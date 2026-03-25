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

#include <fcntl.h>
#include <seastar/core/future.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/seastar.hh>
#include <seastar/websocket/common.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>
#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>
#include <random>
#include <seastar/websocket/parser.hh>
#include <tuple>

namespace seastar::experimental::websocket {

logger websocket_logger("websocket");

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::handle_ping(temporary_buffer<char> buff) {
    return _output_buffer.push_eventually(std::make_tuple(opcodes::PONG, std::move(buff)));
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::handle_pong() {
    // TODO
    return make_ready_future<>();
}

static thread_local std::mt19937 masking_rng{std::random_device{}()};

static uint32_t generate_masking_key() {
    return masking_rng();
}

static void apply_mask(char* data, size_t len, uint32_t masking_key) {
    char mask_bytes[4];
    write_be<uint32_t>(mask_bytes, masking_key);
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask_bytes[i % 4];
    }
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::send_data(opcodes opcode, temporary_buffer<char> buff) {
    char header[14] = {'\x80', 0}; // max: 2 + 8 (extended len) + 4 (mask key)
    size_t header_size = 2;

    header[0] += opcode;

    if ((126 <= buff.size()) && (buff.size() <= std::numeric_limits<uint16_t>::max())) {
        header[1] = 0x7E;
        write_be<uint16_t>(header + 2, buff.size());
        header_size += sizeof(uint16_t);
    } else if (std::numeric_limits<uint16_t>::max() < buff.size()) {
        header[1] = 0x7F;
        write_be<uint64_t>(header + 2, buff.size());
        header_size += sizeof(uint64_t);
    } else {
        header[1] = uint8_t(buff.size());
    }

    if constexpr (is_client) {
        // RFC 6455 §5.3: client frames must be masked
        header[1] |= 0x80; // set mask bit
        uint32_t masking_key = generate_masking_key();
        write_be<uint32_t>(header + header_size, masking_key);
        header_size += sizeof(uint32_t);
        apply_mask(buff.get_write(), buff.size(), masking_key);
    }

    co_await _write_buf.write(header, header_size);
    co_await _write_buf.write(std::move(buff));
    co_await _write_buf.flush();
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::response_loop() {
    return do_until([this] {return stop_response_loop();}, [this] {
        // FIXME: implement error handling
        return _output_buffer.pop_eventually().then([this] (frame_t frame) {
            auto& [opcode, buf] = frame;
            if ((_state == websocket_state::open) || (_state == websocket_state::closing && !_close_sent && opcode == opcodes::CLOSE)) {
                return send_data(opcode, std::move(buf)).then([this, opcode] () {
                        if (opcode == opcodes::CLOSE) {
                           handle_event(connection_event::close_sent);
                           return _write_buf.close();
                        }

                        return make_ready_future();
                    });
            }

            return make_ready_future();
        });
    });
}

template <bool is_client, bool text_frame>
void basic_connection<is_client, text_frame>::shutdown_input() {
    _fd.shutdown_input();
}


template <bool is_client, bool text_frame>
void basic_connection<is_client, text_frame>::handle_event(connection_event event) {
    if (connection_event::reset == event) {
        _state = websocket_state::closed;
        return;
    }
    switch (_state) {
        case websocket_state::connecting:
            SEASTAR_ASSERT(event == connection_event::handshake_done);
            _state = websocket_state::open;
            break;
        case websocket_state::closed:
            SEASTAR_ASSERT(false);
            break;
        case websocket_state::closing:
            if (connection_event::close_sending == event) {
                SEASTAR_ASSERT(!_close_sent);
                SEASTAR_ASSERT(_close_recv);
                return;
            } else if (connection_event::close_sent == event) {
                SEASTAR_ASSERT(!_close_sent);
                _close_sent = true;
            } else if (connection_event::recv_close == event) {
                SEASTAR_ASSERT(!_close_recv);
                _close_recv = true;
            }

            _state = websocket_state::closed;
            break;
        case websocket_state::open:
            if (connection_event::close_sending == event) {
                SEASTAR_ASSERT(!_close_sent);
                _state = websocket_state::closing;
            } else if (connection_event::recv_close == event) {
                SEASTAR_ASSERT(!_close_recv);
                _close_recv = true;
                _state = websocket_state::closing;
            } else {
                _state = websocket_state::closed;
            }
            break;
        }
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::close(bool send_close) {
    return [this, send_close]() {
        if (send_close) {
            handle_event(connection_event::close_sending);
            return _output_buffer.push_eventually(std::make_tuple(opcodes::CLOSE, temporary_buffer<char>(0)));
        } else {
            _state = websocket_state::closed;
            return make_ready_future<>();
        }
    }().finally([this] {
        return when_all_succeed(_input.close(), _output.close()).discard_result();
    });
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::handle_exception(std::exception_ptr e) {
    switch (_state) {
    case websocket_state::connecting:
    case websocket_state::open:
    case websocket_state::closing:
    case websocket_state::closed:
        _state = websocket_state::closed;
        return when_all_succeed(_read_buf.close(), _write_buf.close()).discard_result();
      break;
    }

    return make_ready_future();
}

template <bool is_client, bool text_frame>
bool basic_connection<is_client, text_frame>::stop_read_loop() {
    return _state == websocket_state::closed
                    || (_state == websocket_state::closing && this->_close_recv);
}

template <bool is_client, bool text_frame>
bool basic_connection<is_client, text_frame>::stop_response_loop() {
    return _state == websocket_state::closed
                    || (_state == websocket_state::closing && this->_close_sent);
}

template <bool is_client, bool text_frame>
future<> basic_connection<is_client, text_frame>::read_one() {
    return _read_buf.consume(_websocket_parser).then([this] () mutable {
        if (_websocket_parser.is_valid()) {
            if (_state == websocket_state::closing && _websocket_parser.opcode() == opcodes::CLOSE && !_close_recv) {
                handle_event(connection_event::recv_close);
                return _read_buf.close();
            }
            if (_state != websocket_state::open) {
                return make_ready_future();
            }
            // FIXME: implement error handling
            switch(_websocket_parser.opcode()) {
            // We do not distinguish between these 3 types.
            case opcodes::CONTINUATION:
            case opcodes::TEXT:
            case opcodes::BINARY:
                return _input_buffer.push_eventually(_websocket_parser.result());
            case opcodes::CLOSE:
                websocket_logger.debug("Received close frame.");
                handle_event(connection_event::recv_close);
                // datatracker.ietf.org/doc/html/rfc6455#section-5.5.1
                return close(true);
            case opcodes::PING:
                websocket_logger.debug("Received ping frame.");
                return handle_ping(_websocket_parser.result());
            case opcodes::PONG:
                websocket_logger.debug("Received pong frame.");
                return handle_pong();
            default:
                // Invalid - do nothing.
                ;
            }
        } else if (_websocket_parser.eof()) {
            handle_event(connection_event::reset);
            return close(false);
        }

        throw exception("Parse websocket frame failed");

    });
}

std::string sha1_base64(std::string_view source) {
    unsigned char hash[20];
    SEASTAR_ASSERT(sizeof(hash) == gnutls_hash_get_len(GNUTLS_DIG_SHA1));
    if (int ret = gnutls_hash_fast(GNUTLS_DIG_SHA1, source.data(), source.size(), hash);
        ret != GNUTLS_E_SUCCESS) {
        throw websocket::exception(fmt::format("gnutls_hash_fast: {}", gnutls_strerror(ret)));
    }
    return encode_base64(std::string_view(reinterpret_cast<const char*>(hash), sizeof(hash)));
}

std::string encode_base64(std::string_view source) {
    gnutls_datum_t src_data{
        .data = reinterpret_cast<uint8_t*>(const_cast<char*>(source.data())),
        .size = static_cast<unsigned>(source.size())
    };
    gnutls_datum_t encoded_data;
    if (int ret = gnutls_base64_encode2(&src_data, &encoded_data); ret != GNUTLS_E_SUCCESS) {
        throw websocket::exception(fmt::format("gnutls_base64_encode2: {}", gnutls_strerror(ret)));
    }
    auto free_encoded_data = defer([&] () noexcept { gnutls_free(encoded_data.data); });
    // base64_encoded.data is "unsigned char *"
    return std::string(reinterpret_cast<const char*>(encoded_data.data), encoded_data.size);
}

template class basic_connection<true, false>;
template class basic_connection<true, true>;
template class basic_connection<false, false>;
template class basic_connection<false, true>;

}
