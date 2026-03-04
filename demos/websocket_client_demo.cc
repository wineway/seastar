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

#include <seastar/websocket/client.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>
#include <string_view>

using namespace seastar;
using namespace seastar::experimental;

namespace bpo = boost::program_options;

static logger ws_demo_logger("ws_demo");

int main(int argc, char** argv) {
    seastar::app_template app;
    app.add_options()
        ("host", bpo::value<std::string>()->default_value("fstream.binance.com"),
            "WebSocket server host")
        ("path", bpo::value<std::string>()->default_value("/ws/btcusdt@trade"),
            "WebSocket resource path")
        ("port", bpo::value<uint16_t>()->default_value(443),
            "WebSocket server port")
        ("duration", bpo::value<unsigned>()->default_value(30),
            "Duration in seconds to receive data")
        ("tls", bpo::value<bool>()->default_value(true),
            "Use TLS (wss://)");
    return app.run(argc, argv, [&app]() -> seastar::future<> {
        auto&& config = app.configuration();
        auto host = config["host"].as<std::string>();
        auto path = config["path"].as<std::string>();
        auto port = config["port"].as<uint16_t>();
        auto duration = config["duration"].as<unsigned>();
        auto use_tls = config["tls"].as<bool>();

        return async([=] {
            ws_demo_logger.info("Resolving {}...", host);
            net::hostent e = net::dns::get_host_by_name(host,
                net::inet_address::family::INET).get();
            auto addr = socket_address(e.addr_entries.front().addr, port);
            ws_demo_logger.info("Connecting to {}:{}{} ...", host, port, path);

            websocket::client ws_client;
            auto d = defer([&ws_client] () noexcept {
                ws_client.close().get();
            });

            if (use_tls) {
                auto creds = make_shared<tls::certificate_credentials>();
                creds->set_system_trust().get();
                ws_client.connect(addr, std::move(creds),
                    sstring(path), sstring(host), "",
                    [] (input_stream<char>& in, output_stream<char>& out) -> future<> {
                        return repeat([&in]() {
                            return in.read().then([](temporary_buffer<char> f) {
                                if (f.empty()) {
                                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                                } else {
                                    std::cerr << "receive: " << std::string_view(f.get(), f.size()) << "\n";
                                    return make_ready_future<stop_iteration>(stop_iteration::no);
                                }
                            });
                        });
                    }).get();
            } else {
                ws_client.connect(addr,
                    sstring(path), sstring(host), "",
                    [] (input_stream<char>& in, output_stream<char>& out) -> future<> {
                        return repeat([&in]() {
                            return in.read().then([](temporary_buffer<char> f) {
                                if (f.empty()) {
                                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                                } else {
                                    std::cerr << "receive: " << std::string_view(f.get(), f.size()) << "\n";
                                    return make_ready_future<stop_iteration>(stop_iteration::no);
                                }
                            });
                        });
                    }).get();
            }

            ws_demo_logger.info("Connected! Receiving for {} seconds...",
                duration);
            seastar::sleep_abortable(std::chrono::seconds(duration))
                .handle_exception([] (auto) {}).get();
            ws_demo_logger.info("Done, closing connection.");
        }).handle_exception([] (auto ep) {
            ws_demo_logger.error("Error: {}", ep);
        });
    });
}
