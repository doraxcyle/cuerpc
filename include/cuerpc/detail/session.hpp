/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef CUERPC_SESSION_HPP_
#define CUERPC_SESSION_HPP_

#include <memory>
#include <functional>
#include <queue>
#include <mutex>
#include <vector>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include "cuerpc/detail/common.hpp"
#include "cuerpc/detail/endian.hpp"
#include "cuerpc/detail/protocol.hpp"
#include "cuerpc/detail/noncopyable.hpp"
#include "cuerpc/detail/stub.hpp"

namespace cue {
namespace rpc {
namespace detail {

class session final : public std::enable_shared_from_this<session>, safe_noncopyable {
public:
    session(std::function<void(std::shared_ptr<session>, std::shared_ptr<request>)> handler,
            boost::asio::io_service& engine) noexcept
        : socket_{engine}, heartbeat_check_timer_{engine}, handler_{std::move(handler)} {
    }

    ~session() noexcept = default;

    tcp_socket& socket() noexcept {
        return socket_;
    }

    void start() {
        do_check_heartbeat();
        do_read_match();
    }

    void reply(response&& res) {
        std::unique_lock<std::mutex> lock{write_queue_mutex_};
        write_queue_.emplace(std::move(res));

        if (write_queue_.size() == 1) {
            lock.unlock();
            do_write();
        }
    }

private:
    void close() {
        boost::system::error_code code;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, code);
        socket_.close(code);
    }

    void close_all() {
        heartbeat_check_timer_.cancel();
        close();
    }

    void do_check_heartbeat() {
        heartbeat_check_timer_.cancel();
        heartbeat_check_timer_.expires_from_now(std::chrono::seconds{heartbeat_check_interval_});
        heartbeat_check_timer_.async_wait([this, self = this->shared_from_this()](boost::system::error_code code) {
            if (!code) {
                // heartbeat/request timeout
                close();
            }
        });
    }

    void do_read_match() {
        boost::asio::async_read(
            socket_, boost::asio::buffer(match_),
            [this, self = this->shared_from_this()](boost::system::error_code code, std::size_t bytes_transferred) {
                if (code) {
                    close_all();
                    return;
                }

                if (match_[0] != protocol_match) {
                    return;
                }

                do_read_header();
            });
    }

    void do_read_header() {
        boost::asio::async_read(
            socket_, boost::asio::buffer(request_header_),
            [this, self = this->shared_from_this()](boost::system::error_code code, std::size_t bytes_transferred) {
                if (code) {
                    close_all();
                    return;
                }

                header_ = *reinterpret_cast<request_header*>(request_header_);
                // from big endian
                header_.request_id = from_be(header_.request_id);
                header_.timeout = from_be(header_.timeout);
                header_.payload_length = from_be(header_.payload_length);

                do_check_heartbeat();

                switch (header_.type) {
                case request_type::heartbeat: {
                    response_header header;
                    header.type = request_type::heartbeat;
                    header.request_id = header_.request_id;
                    reply({std::move(header), ""});
                    do_read_match();
                    break;
                }
                case request_type::request:
                case request_type::oneway:
                    do_read_payload();
                    break;
                default:
                    break;
                }
            });
    }

    void do_read_payload() {
        payload_.resize(header_.payload_length);
        boost::asio::async_read(
            socket_, boost::asio::buffer(payload_.data(), header_.payload_length),
            [this, self = this->shared_from_this()](boost::system::error_code code, std::size_t bytes_transferred) {
                if (code) {
                    close_all();
                    return;
                }

                // continue
                do_read_match();

                auto req = std::make_shared<request>();
                req->header = header_;
                req->payload = std::string{payload_.data(), header_.payload_length};
                handler_(shared_from_this(), req);
            });
    }

    void do_write() {
        auto& res = get_response();
        std::vector<boost::asio::const_buffer> buffers;
        // match
        buffers.emplace_back(boost::asio::buffer(&protocol_match, 1));
        // response header
        buffers.emplace_back(boost::asio::buffer(&res.header.version, sizeof(uint8_t)));
        buffers.emplace_back(boost::asio::buffer(&res.header.type, sizeof(uint8_t)));
        buffers.emplace_back(boost::asio::buffer(&res.header.codec, sizeof(uint8_t)));
        res.header.request_id = to_be(res.header.request_id);
        buffers.emplace_back(boost::asio::buffer(&res.header.request_id, sizeof(uint64_t)));
        buffers.emplace_back(boost::asio::buffer(&res.header.code, sizeof(uint8_t)));
        res.header.payload_length = to_be(res.header.payload_length);
        buffers.emplace_back(boost::asio::buffer(&res.header.payload_length, sizeof(uint32_t)));
        // payload
        if (res.header.payload_length > 0) {
            buffers.emplace_back(boost::asio::buffer(res.payload.data(), res.payload.size()));
        }

        boost::asio::async_write(
            socket_, buffers,
            [this, self = this->shared_from_this()](boost::system::error_code code, std::size_t bytes_transferred) {
                if (code) {
                    close_all();
                    return;
                }

                std::unique_lock<std::mutex> lock{write_queue_mutex_};
                write_queue_.pop();
                if (!write_queue_.empty()) {
                    lock.unlock();
                    do_write();
                }
            });
    }

    response& get_response() {
        std::unique_lock<std::mutex> lock{write_queue_mutex_};
        assert(!write_queue_.empty());
        return write_queue_.front();
    }

    tcp_socket socket_;
    boost::asio::steady_timer heartbeat_check_timer_;
    const unsigned heartbeat_check_interval_{90};
    char match_[1];
    char request_header_[sizeof(request_header)];
    request_header header_;
    std::vector<char> payload_;
    std::function<void(std::shared_ptr<session>, std::shared_ptr<request>)> handler_;
    std::queue<response> write_queue_;
    std::mutex write_queue_mutex_;
};

} // namespace detail
} // namespace rpc
} // namespace cue

#endif // CUERPC_SESSION_HPP_
