/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2024 ScyllaDB
 */

#pragma once

#include <exception>
#include <seastar/core/seastar.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/queue.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <seastar/websocket/parser.hh>
#include <tuple>

namespace seastar::experimental::websocket {

using handler_t = std::function<future<>(input_stream<char>&, output_stream<char>&)>;

class server;

/// \defgroup websocket WebSocket
/// \addtogroup websocket
/// @{

/*!
 * \brief an error in handling a WebSocket connection
 */
class exception : public std::exception {
    std::string _msg;
public:
    exception(std::string_view msg) : _msg(msg) {}
    virtual const char* what() const noexcept {
        return _msg.c_str();
    }
};

/*!
 * \brief a server WebSocket connection
 */
template <bool is_client = false, bool text_frame = false>
class basic_connection : public boost::intrusive::list_base_hook<> {
protected:
    using buff_t = temporary_buffer<char>;

    enum class websocket_state {
        connecting,
        open,
        closing,
        closed,
    };

    enum class connection_event {
        handshake_done,
        close_sending,
        close_sent,
        recv_close,
        read_exit,
        write_exit
    };

    /*!
     * \brief Implementation of connection's data source.
     */
    class connection_source_impl final : public data_source_impl {
        queue<buff_t>* data;

    public:
        connection_source_impl(queue<buff_t>* data) : data(data) {}

        virtual future<buff_t> get() override {
            return data->pop_eventually().then_wrapped([](future<buff_t> f){
                try {
                    return make_ready_future<buff_t>(std::move(f.get()));
                } catch(...) {
                    return current_exception_as_future<buff_t>();
                }
            });
        }

        virtual future<> close() override {
            data->push(buff_t(0));
            return make_ready_future<>();
        }
    };

    using frame_t = std::tuple<opcodes, buff_t>;

    /*!
     * \brief Implementation of connection's data sink.
     */
    class connection_sink_impl final : public data_sink_impl {
        queue<frame_t>* data;

        opcodes opcode() {
            return text_frame ? opcodes::TEXT : opcodes::BINARY;
        }
    public:
        connection_sink_impl(queue<frame_t>* data) : data(data) {}

#if SEASTAR_API_LEVEL >= 9
        future<> put(std::span<temporary_buffer<char>> d) override {
            return data_sink_impl::fallback_put(d, [this] (temporary_buffer<char>&& buf) {
                return data->push_eventually(std::make_tuple(opcode(), std::move(buf)));
            });
        }
#else
        virtual future<> put(net::packet d) override {
            net::fragment f = d.frag(0);
            return data->push_eventually(std::make_tuple(opcode(), temporary_buffer<char>{std::move(f.base), f.size}));
        }
#endif

        size_t buffer_size() const noexcept override {
            return data->max_size();
        }

        virtual future<> close() override {
            // use opcodes::INVALID indicate stream was closed
            data->push(std::make_tuple(opcodes::INVALID, buff_t(0)));
            return make_ready_future<>();
        }
    };

    /*!
     * \brief This function processess received PING frame.
     * https://datatracker.ietf.org/doc/html/rfc6455#section-5.5.2
     */
    future<> handle_ping(temporary_buffer<char>);
    /*!
     * \brief This function processess received PONG frame.
     * https://datatracker.ietf.org/doc/html/rfc6455#section-5.5.3
     */
    future<> handle_pong();

    static const size_t PIPE_SIZE = 512;
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    websocket_state _state;
    bool _close_sent = false;
    bool _close_recv = false;
    bool _read_closed = false;
    bool _write_closed = false;

    websocket_parser _websocket_parser;
    queue<temporary_buffer<char>> _input_buffer;
    input_stream<char> _input;
    queue<frame_t> _output_buffer;
    output_stream<char> _output;

    sstring _subprotocol;
    handler_t _handler;
public:
    /*!
     * \param fd established socket used for communication
     * \param is_client if true, this is a client-side connection (sends masked
     *        frames and expects unmasked frames from server)
     */
    basic_connection(connected_socket&& fd)
        : _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
        , _state(websocket_state::connecting)
        , _websocket_parser(!is_client)
        , _input_buffer{PIPE_SIZE}
        , _input(data_source{std::make_unique<connection_source_impl>(&_input_buffer)})
        , _output_buffer{PIPE_SIZE}
        , _output(data_sink{std::make_unique<connection_sink_impl>(&_output_buffer)})
    {
    }

    /*!
     * \brief close the socket
     */
    void shutdown_input();
    future<> close(bool send_close = true);

protected:
    future<> read_one();
    future<> response_loop();
    /*!
     * \brief Enqueues a CLOSE frame to the output buffer and updates the state machine.
     *
     * Transitions the connection state from \c open to \c closing by firing
     * \c connection_event::close_sending, then pushes an empty CLOSE frame onto
     * \c _output_buffer for the response loop to transmit.
     *
     * This is the low-level primitive for initiating or echoing the WebSocket
     * closing handshake. Callers are responsible for ensuring the connection is
     * in the \c open state before calling this method; invoking it in any other
     * state will trigger an assertion failure in \ref handle_event.
     *
     * \note Prefer \ref close over this method for externally-initiated shutdowns,
     *       as \ref close includes the necessary state guard and notifies the
     *       handler via \ref _input.
     */
    future<> send_close();
    /*!
     * \brief Runs the handler and the read loop concurrently.
     *
     * Invokes the user-supplied handler with the connection's input and output
     * streams, while simultaneously reading and dispatching incoming WebSocket
     * frames via \ref read_one. Both tasks must complete before this future
     * resolves.
     */
    future<> read_loop();
    /*!
     * \brief Returns true when the read loop should stop.
     *
     * The read loop stops when the connection is \c closed, or when it is
     * \c closing and a CLOSE frame has already been received from the peer.
     */
    bool stop_read_loop();
    /*!
     * \brief Returns true when the response loop should stop.
     *
     * The response loop stops when the connection is \c closed, or when it is
     * \c closing and a CLOSE frame has already been sent to the peer.
     */
    bool stop_response_loop();
    /*!
     * \brief Drives the WebSocket closing-handshake state machine.
     *
     * Transitions \c _state according to \p event. Valid events per state:
     * - \c connecting: only \c handshake_done (advances to \c open).
     * - \c open: \c close_sending, \c recv_close (both advance to \c closing),
     *   or any other event (advances directly to \c closed).
     * - \c closing: \c close_sent, \c recv_close (advance to \c closed once
     *   both sides have exchanged CLOSE frames), or \c close_sending (no-op).
     * - \c closed: only \c read_exit or \c write_exit.
     */
    void handle_event(connection_event event);
    /*!
     * \brief Packs buff in websocket frame and sends it to the client.
     */
    future<> send_data(opcodes opcode, temporary_buffer<char> buff);
};

using connection = basic_connection<false, false>;

std::string sha1_base64(std::string_view source);
std::string encode_base64(std::string_view source);

extern logger websocket_logger;

/// @}
}
