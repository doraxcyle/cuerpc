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

template <typename _Ret, typename _Tuple>
class method final {
public:
    explicit method(_Tuple&& t) noexcept : t_{std::move(t)} {
    }

    template <typename _Name>
    method(_Tuple&& t, _Name&& name) noexcept : t_{std::move(t)}, name_{std::forward<_Name>(name)} {
    }

    std::string name() const noexcept {
        assert(!name_.empty());
        return std::move(name_);
    }

    template <typename _Name>
    void name(_Name&& name) {
        name_ = std::forward<_Name>(name);
    }

    _Tuple args() const noexcept {
        return std::move(t_);
    }

private:
    _Tuple t_;
    std::string name_;
};

template <typename _Ty>
class register_method;

template <typename _Ret, typename... _Args>
class register_method<_Ret(_Args...)> final {
public:
    template <typename _Name, typename = std::enable_if_t<!std::is_same<_Name, register_method>{}>>
    explicit register_method(_Name&& name) noexcept : name_{std::forward<_Name>(name)} {
    }

    decltype(auto) operator()(_Args... args) const noexcept {
        auto args_tuple = std::forward_as_tuple(args...);
        return method<_Ret, std::tuple<std::decay_t<_Args>...>>{std::move(args_tuple), name_};
    }

private:
    std::string name_;
};

template <typename _Ret, typename... _Args>
decltype(auto) make_method(_Args&&... args) noexcept {
    auto args_tuple = std::forward_as_tuple(std::forward<_Args>(args)...);
    return method<_Ret, std::tuple<std::decay_t<_Args>...>>{std::move(args_tuple)};
}

class client final : safe_noncopyable {
public:
    template <typename _Host>
    client(_Host&& host, unsigned short port) noexcept
        : host_{std::forward<_Host>(host)},
          port_{port},
          engine_work_{engine_},
          socket_{engine_},
          heartbeat_timer_{engine_} {
    }

    bool ready(std::uint32_t milliseconds = 0) {
        if (ready_) {
            return true;
        }

        std::call_once(flag_, [this, milliseconds]() {
            if (milliseconds > 0) {
                reconnect_max_count_ = milliseconds / reconnect_default_timeout_;
            }
            do_connect();
        });

        connect_wait_event_.wait();
        return ready_;
    }

    void run() {
        engine_.run();
    }

    std::size_t run_one() {
        return engine_.run_one();
    }

    bool stopped() const {
        return engine_.stopped();
    }

    void stop() {
        engine_.stop();
    }

    template <std::uint32_t _Timeout = 0, typename... _Args>
    auto invoke(_Args&&... args) {
        return invoke_impl<_Timeout>(std::forward<_Args>(args)...);
    }

    template <std::uint32_t _Timeout = 0, typename... _Args>
    auto async_invoke(_Args&&... args) {
        return async_invoke_impl<_Timeout>(std::forward<_Args>(args)...);
    }

    template <typename... _Args>
    void invoke_oneway(_Args&&... args) {
        invoke_oneway_impl(std::forward<_Args>(args)...);
    }

private:
    template <std::uint32_t _Timeout, typename _Name, typename _Ret, typename _Tuple>
    auto invoke_impl(_Name&& name, method<_Ret, _Tuple>&& method) {
        method.name(std::forward<_Name>(name));
        return invoke_impl<_Timeout>(std::move(method));
    }

    template <std::uint32_t _Timeout, typename _Ret, typename _Tuple>
    auto invoke_impl(method<_Ret, _Tuple>&& method) {
        auto result = async_invoke<_Timeout>(std::move(method), use_std_future);
        if (_Timeout == 0) {
            result.wait();
        } else {
            const auto status = result.wait_for(std::chrono::milliseconds{_Timeout});
            if (status == std::future_status::deferred) {
                throw invoke_exception{"wait result deferred"};
            }
            if (status == std::future_status::timeout) {
                throw invoke_exception{"wait result timeout"};
            }
        }

        return result.get();
    }

    template <std::uint32_t _Timeout, typename _Name, typename _Ret, typename _Tuple, typename _Func>
    auto async_invoke_impl(_Name&& name, method<_Ret, _Tuple>&& method, _Func&& func) {
        method.name(std::forward<_Name>(name));
        return async_invoke_impl<_Timeout>(std::move(method), std::forward<_Func>(func), std::is_void<_Ret>{});
    }

    template <std::uint32_t _Timeout, typename _Ret, typename _Tuple, typename _Func>
    auto async_invoke_impl(method<_Ret, _Tuple>&& method, _Func&& func) {
        return async_invoke_impl<_Timeout>(std::move(method), std::forward<_Func>(func), std::is_void<_Ret>{});
    }

    template <std::uint32_t _Timeout, typename _Ret, typename _Tuple, typename _Func>
    auto async_invoke_impl(method<_Ret, _Tuple>&& method, _Func&& func, std::true_type) {
        using adapter_type = detail::callback_adapter<std::decay_t<_Func>, void(error_code)>;
        auto adapter = adapter_type::traits(std::forward<_Func>(func));
        const std::uint64_t request_id{request_id_++};
        SCOPE_BLOCK {
            auto callback = [func = std::move(std::get<0>(adapter))](error_code code, std::string&& payload) {
                func(code);
            };
            auto func_adapter =
                std::make_shared<callback_adapter>(engine_, request_id, _Timeout, std::move(callback),
                                                   std::bind(&client::release, this, std::placeholders::_1));
            func_adapter->run();
            std::unique_lock<std::mutex> lock{invokes_mutex_};
            invokes_.emplace(request_id, std::move(func_adapter));
        }
        request(request_id, _Timeout, detail::stub::pack(method.name(), method.args()), detail::request_type::request);
        return std::get<1>(adapter).get();
    }

    template <std::uint32_t _Timeout, typename _Ret, typename _Tuple, typename _Func>
    auto async_invoke_impl(method<_Ret, _Tuple>&& method, _Func&& func, std::false_type) {
        using adapter_type = detail::callback_adapter<std::decay_t<_Func>, void(error_code, _Ret)>;
        auto adapter = adapter_type::traits(std::forward<_Func>(func));
        const std::uint64_t request_id{request_id_++};
        SCOPE_BLOCK {
            auto callback = [func = std::move(std::get<0>(adapter))](error_code code, std::string&& payload) {
                if (code != error_code::success || payload.empty()) {
                    func(code, _Ret{});
                } else {
                    _Ret result;
                    try {
                        result = std::get<0>(detail::stub::unpack<std::tuple<_Ret>>(payload));
                    } catch (...) {
                        func(error_code::exception, _Ret{});
                        return;
                    }
                    func(code, std::move(result));
                }
            };
            auto func_adapter =
                std::make_shared<callback_adapter>(engine_, request_id, _Timeout, std::move(callback),
                                                   std::bind(&client::release, this, std::placeholders::_1));
            func_adapter->run();
            std::unique_lock<std::mutex> lock{invokes_mutex_};
            invokes_.emplace(request_id, std::move(func_adapter));
        }
        request(request_id, _Timeout, detail::stub::pack(method.name(), method.args()), detail::request_type::request);
        return std::get<1>(adapter).get();
    }

    template <std::uint32_t _Timeout, typename _Name, typename _Ret, typename _Tuple, typename _Ty, typename _Func,
              typename _Self>
    void async_invoke_impl(_Name&& name, method<_Ret, _Tuple>&& method, _Func _Ty::*func, _Self self) {
        method.name(std::forward<_Name>(name));
        async_invoke_impl<_Timeout>(std::move(method), func, self);
    }

    template <std::uint32_t _Timeout, typename _Name, typename _Ret, typename _Tuple, typename _Ty, typename _Func>
    void async_invoke_impl(_Name&& name, method<_Ret, _Tuple>&& method, _Func _Ty::*func) {
        method.name(std::forward<_Name>(name));
        async_invoke_impl<_Timeout>(std::move(method), func, static_cast<_Ty*>(nullptr));
    }

    template <std::uint32_t _Timeout, typename _Ret, typename _Tuple, typename _Ty, typename _Func>
    void async_invoke_impl(method<_Ret, _Tuple>&& method, _Func _Ty::*func) {
        async_invoke_impl<_Timeout>(std::move(method), func, static_cast<_Ty*>(nullptr));
    }

    template <std::uint32_t _Timeout, typename _Ret, typename _Tuple, typename _Ty, typename _Func, typename _Self,
              typename = std::enable_if_t<std::is_void<_Ret>{}>>
    std::enable_if_t<detail::is_void_result<_Func>{}> async_invoke_impl(method<_Ret, _Tuple>&& method, _Func _Ty::*func,
                                                                        _Self self) {
        async_invoke_impl<_Timeout>(std::move(method), [func, self](error_code code) {
            if (self) {
                (self->*func)(code);
            } else {
                (_Ty{}.*func)(code);
            }
        });
    }

    template <std::uint32_t _Timeout, typename _Ret, typename _Tuple, typename _Ty, typename _Func, typename _Self,
              typename = std::enable_if_t<!std::is_void<_Ret>{}>>
    std::enable_if_t<detail::is_not_void_result<_Ret, _Func>{}> async_invoke_impl(method<_Ret, _Tuple>&& method,
                                                                                  _Func _Ty::*func, _Self self) {
        async_invoke_impl<_Timeout>(std::move(method), [func, self](error_code code, _Ret&& result) {
            if (self) {
                (self->*func)(code, std::move(result));
            } else {
                (_Ty{}.*func)(code, std::move(result));
            }
        });
    }

    template <typename _Name, typename _Ret, typename _Tuple>
    void invoke_oneway_impl(_Name&& name, method<_Ret, _Tuple>&& method) {
        method.name(std::forward<_Name>(name));
        invoke_oneway_impl(std::move(method));
    }

    template <typename _Ret, typename _Tuple>
    void invoke_oneway_impl(method<_Ret, _Tuple>&& method) {
        request(request_id_++, 0, detail::stub::pack(method.name(), method.args()), detail::request_type::oneway);
    }

    void do_connect() {
        assert(port_ != 0);
        const auto endpoint = boost::asio::ip::address::from_string(host_);
        socket_.async_connect({endpoint, port_}, [this](boost::system::error_code code) {
            if (code) {
                ready_ = false;
                if (++reconnect_count_ > reconnect_max_count_) {
                    connect_wait_event_.stop();
                    stop();
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds{reconnect_default_timeout_});
                    do_reconnect();
                }
            } else {
                ready_ = true;
                reconnect_count_ = 0;
                do_read_match();
                connect_wait_event_.stop();
                do_heartbeat();
                // for reconnect success
                std::unique_lock<std::mutex> lock{write_queue_mutex_};
                if (!write_queue_.empty()) {
                    lock.unlock();
                    do_write();
                }
            }
        });
    }

    void do_reconnect() {
        socket_ = boost::asio::ip::tcp::socket{engine_};
        do_connect();
    }

    void close() {
        ready_ = false;
        boost::system::error_code code;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, code);
        socket_.close(code);
        do_reconnect();
    }

    void close_all() {
        heartbeat_timer_.cancel();
        close();
    }

    void do_heartbeat() {
        heartbeat_timer_.expires_from_now(std::chrono::seconds{heartbeat_interval_});
        heartbeat_timer_.async_wait([this](boost::system::error_code code) {
            if (!code) {
                heartbeat_check_response_ = false;
                request(request_id_++, 0, "", detail::request_type::heartbeat);
            }

            if (!heartbeat_check_response_) {
                if (++heartbeat_failed_count_ >= heartbeat_failed_max_count_) {
                    close();
                } else {
                    do_heartbeat();
                }
            }
        });
    }

    void redo_heartbeat() {
        heartbeat_failed_count_ = 0;
        heartbeat_check_response_ = true;
        heartbeat_timer_.cancel();
        do_heartbeat();
    }

    void do_read_match() {
        boost::asio::async_read(socket_, boost::asio::buffer(match_),
                                [this](boost::system::error_code code, std::size_t bytes_transferred) {
                                    if (code) {
                                        close_all();
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
                                        close_all();
                                        return;
                                    }

                                    redo_heartbeat();

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
                                        close_all();
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

    void request(std::uint64_t request_id, std::uint32_t timeout, std::string&& payload,
                 detail::request_type request_type) {
        detail::request req;
        req.header.type = request_type;
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
        buffers.emplace_back(boost::asio::buffer(&req.header.version, sizeof(std::uint8_t)));
        buffers.emplace_back(boost::asio::buffer(&req.header.type, sizeof(std::uint8_t)));
        buffers.emplace_back(boost::asio::buffer(&req.header.codec, sizeof(std::uint8_t)));
        req.header.request_id = detail::to_be(req.header.request_id);
        buffers.emplace_back(boost::asio::buffer(&req.header.request_id, sizeof(std::uint64_t)));
        req.header.timeout = detail::to_be(req.header.timeout);
        buffers.emplace_back(boost::asio::buffer(&req.header.timeout, sizeof(std::uint32_t)));
        req.header.payload_length = detail::to_be(req.header.payload_length);
        buffers.emplace_back(boost::asio::buffer(&req.header.payload_length, sizeof(std::uint32_t)));
        // payload
        if (req.header.payload_length > 0) {
            buffers.emplace_back(boost::asio::buffer(req.payload.data(), req.payload.size()));
        }

        boost::asio::async_write(socket_, buffers,
                                 [this](boost::system::error_code code, std::size_t bytes_transferred) {
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

    detail::request& get_request() {
        std::unique_lock<std::mutex> lock{write_queue_mutex_};
        assert(!write_queue_.empty());
        return write_queue_.front();
    }

    void release(std::uint64_t request_id) {
        std::unique_lock<std::mutex> lock{write_queue_mutex_};
        invokes_.erase(request_id);
    }

    using result_handler = std::function<void(error_code, std::string&&)>;

    class callback_adapter final : safe_noncopyable, public std::enable_shared_from_this<callback_adapter> {
    public:
        callback_adapter(boost::asio::io_service& engine, std::uint64_t request_id, std::uint32_t timeout,
                         result_handler handler, std::function<void(std::uint64_t)> releaser) noexcept
            : engine_{engine},
              request_id_{request_id},
              timeout_{timeout},
              handler_{std::move(handler)},
              releaser_{std::move(releaser)} {
        }

        void run() {
            if (timeout_ > 0) {
                timer_ = std::make_unique<boost::asio::steady_timer>(engine_);
                timer_->expires_from_now(std::chrono::milliseconds{timeout_});
                timer_->async_wait([this, self = this->shared_from_this()](boost::system::error_code code) {
                    if (!code) {
                        releaser_(request_id_);
                    }
                });
            }
        }

        void stop() {
            if (timeout_ > 0) {
                timer_->cancel();
            }
        }

        void handle(error_code code, std::string&& payload) {
            handler_(code, std::move(payload));
        }

    private:
        boost::asio::io_service& engine_;
        std::uint64_t request_id_;
        std::uint32_t timeout_{0};
        result_handler handler_;
        std::function<void(std::uint64_t)> releaser_;
        std::unique_ptr<boost::asio::steady_timer> timer_;
    };

    std::string host_;
    unsigned short port_{0};
    boost::asio::io_service engine_;
    boost::asio::io_service::work engine_work_;
    boost::asio::ip::tcp::socket socket_;
    std::once_flag flag_;
    unsigned reconnect_max_count_{5};
    unsigned reconnect_count_{0};
    const unsigned reconnect_default_timeout_{1000};
    std::atomic_bool ready_{false};
    detail::wait_event connect_wait_event_;
    boost::asio::steady_timer heartbeat_timer_;
    const unsigned heartbeat_interval_{15};
    bool heartbeat_check_response_{true};
    unsigned heartbeat_failed_count_{0};
    const unsigned heartbeat_failed_max_count_{3};
    char match_[1];
    char response_header_[sizeof(detail::response_header)];
    detail::response_header header_;
    std::vector<char> payload_;
    std::unordered_map<std::uint64_t, std::shared_ptr<callback_adapter>> invokes_;
    std::mutex invokes_mutex_;
    std::atomic<std::uint64_t> request_id_{0};
    std::queue<detail::request> write_queue_;
    std::mutex write_queue_mutex_;
};

} // namespace rpc
} // namespace cue

#endif // CUERPC_CLIENT_HPP_
