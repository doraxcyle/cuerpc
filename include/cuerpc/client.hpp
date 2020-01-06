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

#ifndef CUERPC_CLIENT_HPP_
#define CUERPC_CLIENT_HPP_

#include <tuple>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <memory>
#include <queue>
#include <vector>
#include <future>
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include "cuerpc/detail/noncopyable.hpp"
#include "cuerpc/detail/common.hpp"
#include "cuerpc/detail/endian.hpp"
#include "cuerpc/detail/use_future.hpp"
#include "cuerpc/detail/use_awaitable.hpp"
#include "cuerpc/detail/wait_event.hpp"
#include "cuerpc/detail/stub.hpp"

namespace cue {
namespace rpc {

template <typename R, typename Tuple>
class method final {
public:
    explicit method(Tuple&& t) noexcept : t_{std::move(t)} {
    }

    template <typename Name>
    method(Tuple&& t, Name&& name) noexcept : t_{std::move(t)}, name_{std::forward<Name>(name)} {
    }

    const std::string& name() const noexcept {
        assert(!name_.empty());
        return name_;
    }

    template <typename Name>
    void name(Name&& name) {
        name_ = std::forward<Name>(name);
    }

    const Tuple& args() const noexcept {
        return t_;
    }

private:
    Tuple t_;
    std::string name_;
};

template <typename T>
class register_method;

template <typename R, typename... Args>
class register_method<R(Args...)> final {
public:
    template <typename Name, typename = std::enable_if_t<!std::is_same<Name, register_method>::value>>
    explicit register_method(Name&& name) noexcept : name_{std::forward<Name>(name)} {
    }

    decltype(auto) operator()(Args... args) const noexcept {
        auto args_tuple = std::forward_as_tuple(args...);
        return method<R, std::tuple<std::decay_t<Args>...>>{std::move(args_tuple), name_};
    }

private:
    std::string name_;
};

template <typename R, typename... Args>
decltype(auto) make_method(Args&&... args) noexcept {
    auto args_tuple = std::forward_as_tuple(std::forward<Args>(args)...);
    return method<R, std::tuple<std::decay_t<Args>...>>{std::move(args_tuple)};
}

class client final : safe_noncopyable {
public:
    template <typename Host, typename = std::enable_if_t<!std::is_same<Host, client>::value>>
    client(Host&& host, unsigned short port) noexcept
        : host_{std::forward<Host>(host)}, port_{port}, engine_work_{engine_}, socket_{engine_} {
        thread_ = std::make_unique<std::thread>([this]() { engine_.run(); });
    }

    bool ready(uint32_t milliseconds = 0) {
        if (ready_) {
            return true;
        }

        do_connect();
        if (milliseconds) {
            return connect_wait_event_.wait_for(milliseconds);
        }

        connect_wait_event_.wait();
        return true;
    }

    void run() {
        if (thread_->joinable()) {
            thread_->join();
        }
    }

    template <uint32_t Timeout = 0, typename... Args>
    auto invoke(Args&&... args) {
        return invoke_impl<Timeout>(std::forward<Args>(args)...);
    }

    template <uint32_t Timeout = 0, typename... Args>
    auto async_invoke(Args&&... args) {
        return async_invoke_impl<Timeout>(std::forward<Args>(args)...);
    }

    template <typename... Args>
    void invoke_oneway(Args&&... args) {
        invoke_oneway_impl(std::forward<Args>(args)...);
    }

private:
    template <uint32_t Timeout, typename Name, typename R, typename Tuple>
    auto invoke_impl(Name&& name, method<R, Tuple>&& method) {
        method.name(std::forward<Name>(name));
        return invoke_impl<Timeout>(std::move(method));
    }

    template <uint32_t Timeout, typename R, typename Tuple>
    auto invoke_impl(method<R, Tuple>&& method) {
        auto result = async_invoke<Timeout>(std::move(method), use_std_future);
        if (Timeout == 0) {
            result.wait();
        } else {
            const auto status = result.wait_for(std::chrono::milliseconds{Timeout});
            if (status == std::future_status::deferred) {
                throw invoke_exception{"wait result deferred"};
            }
            if (status == std::future_status::timeout) {
                throw invoke_exception{"wait result timeout"};
            }
        }

        return result.get();
    }

    template <uint32_t Timeout, typename Name, typename R, typename Tuple, typename Func>
    auto async_invoke_impl(Name&& name, method<R, Tuple>&& method, Func&& func) {
        method.name(std::forward<Name>(name));
        return async_invoke_impl<Timeout>(std::move(method), std::forward<Func>(func), std::is_void<R>{});
    }

    template <uint32_t Timeout, typename R, typename Tuple, typename Func>
    auto async_invoke_impl(method<R, Tuple>&& method, Func&& func) {
        return async_invoke_impl<Timeout>(std::move(method), std::forward<Func>(func), std::is_void<R>{});
    }

    template <uint32_t Timeout, typename R, typename Tuple, typename Func>
    auto async_invoke_impl(method<R, Tuple>&& method, Func&& func, std::true_type) {
        using adapter_type = detail::callback_adapter<std::decay_t<Func>, void(error_code)>;
        auto adapter = adapter_type::traits(std::forward<Func>(func));
        const uint64_t request_id{request_id_++};
        SCOPE_BLOCK {
            auto callback = [func = std::move(std::get<0>(adapter))](error_code code, std::string&& payload) {
                func(code);
            };
            auto func_adapter =
                std::make_shared<callback_adapter>(engine_, request_id, Timeout, std::move(callback),
                                                   std::bind(&client::release, this, std::placeholders::_1));
            func_adapter->start();
            std::unique_lock<std::mutex> lock{invokes_mutex_};
            invokes_.emplace(request_id, std::move(func_adapter));
        }
        request(request_id, Timeout, detail::stub::pack(method.name(), method.args()));
        return std::get<1>(adapter).get();
    }

    template <uint32_t Timeout, typename R, typename Tuple, typename Func>
    auto async_invoke_impl(method<R, Tuple>&& method, Func&& func, std::false_type) {
        using adapter_type = detail::callback_adapter<std::decay_t<Func>, void(error_code, R)>;
        auto adapter = adapter_type::traits(std::forward<Func>(func));
        const uint64_t request_id{request_id_++};
        SCOPE_BLOCK {
            auto callback = [func = std::move(std::get<0>(adapter))](error_code code, std::string&& payload) {
                if (code != error_code::success || payload.empty()) {
                    func(code, R{});
                } else {
                    R result;
                    try {
                        result = std::get<0>(detail::stub::unpack<std::tuple<R>>(payload));
                    } catch (...) {
                        func(error_code::exception, R{});
                        return;
                    }
                    func(code, std::move(result));
                }
            };
            auto func_adapter =
                std::make_shared<callback_adapter>(engine_, request_id, Timeout, std::move(callback),
                                                   std::bind(&client::release, this, std::placeholders::_1));
            func_adapter->start();
            std::unique_lock<std::mutex> lock{invokes_mutex_};
            invokes_.emplace(request_id, std::move(func_adapter));
        }
        request(request_id, Timeout, detail::stub::pack(method.name(), method.args()));
        return std::get<1>(adapter).get();
    }

    template <uint32_t Timeout, typename Name, typename R, typename Tuple, typename T, typename Func, typename Self>
    void async_invoke_impl(Name&& name, method<R, Tuple>&& method, Func T::*func, Self self) {
        method.name(std::forward<Name>(name));
        async_invoke_impl<Timeout>(std::move(method), func, self);
    }

    template <uint32_t Timeout, typename Name, typename R, typename Tuple, typename T, typename Func>
    void async_invoke_impl(Name&& name, method<R, Tuple>&& method, Func T::*func) {
        method.name(std::forward<Name>(name));
        async_invoke_impl<Timeout>(std::move(method), func, static_cast<T*>(nullptr));
    }

    template <uint32_t Timeout, typename R, typename Tuple, typename T, typename Func>
    void async_invoke_impl(method<R, Tuple>&& method, Func T::*func) {
        async_invoke_impl<Timeout>(std::move(method), func, static_cast<T*>(nullptr));
    }

    template <uint32_t Timeout, typename R, typename Tuple, typename T, typename Func, typename Self,
              typename = std::enable_if_t<std::is_void<R>::value>>
    std::enable_if_t<detail::is_void_result<Func>::value> async_invoke_impl(method<R, Tuple>&& method, Func T::*func,
                                                                            Self self) {
        async_invoke_impl<Timeout>(std::move(method), [func, self](error_code code) {
            if (self) {
                (self->*func)(code);
            } else {
                (T{}.*func)(code);
            }
        });
    }

    template <uint32_t Timeout, typename R, typename Tuple, typename T, typename Func, typename Self,
              typename = std::enable_if_t<!std::is_void<R>::value>>
    std::enable_if_t<detail::is_not_void_result<R, Func>::value> async_invoke_impl(method<R, Tuple>&& method,
                                                                                   Func T::*func, Self self) {
        async_invoke_impl<Timeout>(std::move(method), [func, self](error_code code, R result) {
            if (self) {
                (self->*func)(code, result);
            } else {
                (T{}.*func)(code, result);
            }
        });
    }

    template <typename Name, typename R, typename Tuple>
    void invoke_oneway_impl(Name&& name, method<R, Tuple>&& method) {
        method.name(std::forward<Name>(name));
        invoke_oneway_impl(std::move(method));
    }

    template <typename R, typename Tuple>
    void invoke_oneway_impl(method<R, Tuple>&& method) {
        request(request_id_++, 0, detail::stub::pack(method.name(), method.args()), true);
    }

    void do_connect() {
        assert(port_ != 0);
        const auto endpoint = boost::asio::ip::address::from_string(host_);
        socket_.async_connect({endpoint, port_}, [this](boost::system::error_code code) {
            if (code) {
                ready_ = false;
                do_connect();
            }

            ready_ = true;
            do_read_match();
            connect_wait_event_.stop();
        });
    }

    void do_read_match() {
        boost::asio::async_read(socket_, boost::asio::buffer(match_),
                                [this](boost::system::error_code code, std::size_t bytes_transferred) {
                                    if (code) {
                                        return;
                                    }

                                    if (match_[0] != detail::protocol_match) {
                                        return;
                                    }

                                    do_read_header();
                                });
    }

    void do_read_header() {
        boost::asio::async_read(socket_, boost::asio::buffer(response_header_),
                                [this](boost::system::error_code code, std::size_t bytes_transferred) {
                                    if (code) {
                                        return;
                                    }

                                    header_ = *reinterpret_cast<detail::response_header*>(response_header_);
                                    // from big endian
                                    header_.request_id = detail::from_be(header_.request_id);
                                    header_.payload_length = detail::from_be(header_.payload_length);

                                    if (header_.payload_length == 0) {
                                        handle("");
                                        // continue
                                        do_read_match();
                                    } else {
                                        do_read_payload();
                                    }
                                });
    }

    void do_read_payload() {
        payload_.resize(header_.payload_length);
        boost::asio::async_read(socket_, boost::asio::buffer(payload_.data(), header_.payload_length),
                                [this](boost::system::error_code code, std::size_t bytes_transferred) {
                                    if (code) {
                                        return;
                                    }

                                    handle({payload_.data(), payload_.size()});

                                    // continue
                                    do_read_match();
                                });
    }

    void handle(std::string&& payload) {
        std::shared_ptr<callback_adapter> handler;
        SCOPE_BLOCK {
            std::unique_lock<std::mutex> lock{invokes_mutex_};
            auto it = invokes_.find(header_.request_id);
            if (it == invokes_.end()) {
                return;
            }
            handler = std::move(it->second);
        }
        if (handler) {
            handler->stop();
            handler->handle(static_cast<error_code>(header_.code), std::move(payload));
        }
        std::unique_lock<std::mutex> lock{invokes_mutex_};
        invokes_.erase(header_.request_id);
    }

    void request(uint64_t request_id, uint32_t timeout, std::string&& payload, bool oneway = false) {
        assert(!payload.empty());
        detail::request req;
        req.header.type = oneway ? detail::request_type::oneway : detail::request_type::request;
        req.header.request_id = request_id;
        req.header.timeout = timeout;
        req.header.payload_length = payload.size();
        req.payload = std::move(payload);
        std::unique_lock<std::mutex> lock{write_queue_mutex_};
        write_queue_.emplace(std::move(req));

        if (write_queue_.size() == 1) {
            lock.unlock();
            do_write();
        }
    }

    void do_write() {
        auto& req = get_request();
        std::vector<boost::asio::const_buffer> buffers;
        // match
        buffers.emplace_back(boost::asio::buffer(&detail::protocol_match, 1));
        // response header
        buffers.emplace_back(boost::asio::buffer(&req.header.version, sizeof(uint8_t)));
        buffers.emplace_back(boost::asio::buffer(&req.header.type, sizeof(uint8_t)));
        buffers.emplace_back(boost::asio::buffer(&req.header.codec, sizeof(uint8_t)));
        req.header.request_id = detail::to_be(req.header.request_id);
        buffers.emplace_back(boost::asio::buffer(&req.header.request_id, sizeof(uint64_t)));
        req.header.timeout = detail::to_be(req.header.timeout);
        buffers.emplace_back(boost::asio::buffer(&req.header.timeout, sizeof(uint32_t)));
        req.header.payload_length = detail::to_be(req.header.payload_length);
        buffers.emplace_back(boost::asio::buffer(&req.header.payload_length, sizeof(uint32_t)));
        // payload
        if (req.header.payload_length > 0) {
            buffers.emplace_back(boost::asio::buffer(req.payload.data(), req.payload.size()));
        }

        boost::asio::async_write(socket_, buffers,
                                 [this](boost::system::error_code code, std::size_t bytes_transferred) {
                                     if (code) {
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

    detail::request& get_request() {
        std::unique_lock<std::mutex> lock{write_queue_mutex_};
        assert(!write_queue_.empty());
        return write_queue_.front();
    }

    void release(uint64_t request_id) {
        std::unique_lock<std::mutex> lock{write_queue_mutex_};
        invokes_.erase(request_id);
    }

    using result_handler = std::function<void(error_code, std::string&&)>;

    class callback_adapter final : safe_noncopyable, public std::enable_shared_from_this<callback_adapter> {
    public:
        callback_adapter(boost::asio::io_service& engine, uint64_t request_id, uint32_t timeout, result_handler handler,
                         std::function<void(uint64_t)> releaser) noexcept
            : request_id_{request_id},
              timeout_{timeout},
              handler_{std::move(handler)},
              releaser_{std::move(releaser)},
              timer_{engine} {
        }

        void start() {
            if (timeout_ > 0) {
                timer_.expires_from_now(std::chrono::milliseconds{timeout_});
                timer_.async_wait([this, self = this->shared_from_this()](boost::system::error_code code) {
                    if (!code) {
                        releaser_(request_id_);
                    }
                });
            }
        }

        void stop() {
            if (timeout_ > 0) {
                timer_.cancel();
            }
        }

        void handle(error_code code, std::string&& payload) {
            handler_(code, std::move(payload));
        }

    private:
        uint64_t request_id_;
        uint32_t timeout_{0};
        result_handler handler_;
        std::function<void(uint64_t)> releaser_;
        boost::asio::steady_timer timer_;
    };

    std::string host_;
    unsigned short port_{0};
    boost::asio::io_service engine_;
    boost::asio::io_service::work engine_work_;
    boost::asio::ip::tcp::socket socket_;
    std::unique_ptr<std::thread> thread_;
    std::atomic_bool ready_{false};
    detail::wait_event connect_wait_event_;
    char match_[1];
    char response_header_[sizeof(detail::response_header)];
    detail::response_header header_;
    std::vector<char> payload_;
    std::unordered_map<uint64_t, std::shared_ptr<callback_adapter>> invokes_;
    std::mutex invokes_mutex_;
    std::atomic<uint64_t> request_id_{0};
    std::queue<detail::request> write_queue_;
    std::mutex write_queue_mutex_;
};

} // namespace rpc
} // namespace cue

#endif // CUERPC_CLIENT_HPP_
