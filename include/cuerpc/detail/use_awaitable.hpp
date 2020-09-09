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

#ifndef CUERPC_USE_AWAITABLE_HPP_
#define CUERPC_USE_AWAITABLE_HPP_

#if defined(ENABLE_CO_AWAIT)

#include <atomic>
#include <experimental/coroutine>

#include "cuerpc/detail/use_future.hpp"

namespace cue {
namespace rpc {

namespace detail {

template <typename T>
class awaitable_promise;

} // namespace detail

template <typename T>
class awaitable_future {
public:
    using promise_type = detail::awaitable_promise<T>;

    awaitable_future() noexcept = default;
    awaitable_future(const awaitable_future&) = delete;
    awaitable_future& operator=(const awaitable_future&) = delete;

    awaitable_future(awaitable_future&& rhs) noexcept {
        if (std::addressof(rhs) != this) {
            promise_ = rhs.promise_;
            rhs.promise_ = nullptr;
        }
    }

    awaitable_future& operator=(awaitable_future&& rhs) noexcept {
        if (std::addressof(rhs) != this) {
            promise_ = rhs.promise_;
            rhs.promise_ = nullptr;
        }
        return *this;
    }

    // for coroutine
    bool await_ready() const noexcept {
        return promise_->ready();
    }

    // for coroutine
    void await_suspend(std::experimental::coroutine_handle<> handle) noexcept {
        promise_->suspend(std::move(handle));
    }

    // for coroutine
    T await_resume() noexcept {
        return promise_->get();
    }

private:
    template <typename>
    friend class detail::awaitable_promise;

    explicit awaitable_future(detail::awaitable_promise<T>* promise) noexcept : promise_{promise} {
    }

    detail::awaitable_promise<T>* promise_{nullptr};
};

namespace detail {

class awaitable_promise_base {
public:
    awaitable_promise_base() noexcept : state_{std::make_unique<state>()} {
    }

    awaitable_promise_base(const awaitable_promise_base&) = delete;
    awaitable_promise_base& operator=(const awaitable_promise_base&) = delete;

    awaitable_promise_base(awaitable_promise_base&& rhs) noexcept {
        assign_rv(std::move(rhs));
    }

    awaitable_promise_base& operator=(awaitable_promise_base&& rhs) noexcept {
        assign_rv(std::move(rhs));
        return *this;
    }

    void set_exception(std::exception_ptr exception) noexcept {
        std::call_once(state_->flag(), [this, exception = std::move(exception)]() {
            exception_ = std::move(exception);
            state_->ready(true);
            handle_.resume();
        });
    }

    bool ready() const noexcept {
        return state_->ready();
    }

    void suspend(std::experimental::coroutine_handle<> handle) noexcept {
        handle_ = std::move(handle);
    }

    // for coroutine
    auto initial_suspend() noexcept {
        return std::experimental::suspend_never{};
    }

    // for coroutine
    auto final_suspend() noexcept {
        return std::experimental::suspend_always{};
    }

    // for coroutine
    void unhandled_exception() {
        set_exception(std::current_exception());
    }

    void rethrow_exception() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    void swap(awaitable_promise_base& other) noexcept {
        if (this != std::addressof(other)) {
            std::swap(handle_, other.handle_);
            std::swap(exception_, other.exception_);
            std::swap(state_, other.state_);
        }
    }

protected:
    struct state final {
        bool ready() const noexcept {
            return ready_;
        }

        void ready(bool ready) noexcept {
            ready_ = ready;
        }

        std::once_flag& flag() noexcept {
            return flag_;
        }

    private:
        std::once_flag flag_;
        std::atomic_bool ready_{false};
    };

    std::experimental::coroutine_handle<> handle_;
    std::exception_ptr exception_{nullptr};
    std::unique_ptr<state> state_{nullptr};

private:
    void assign_rv(awaitable_promise_base&& other) noexcept {
        if (this != std::addressof(other)) {
            handle_ = nullptr;
            exception_ = nullptr;
            state_.reset();
            swap(other);
        }
    }
};

template <typename T>
class awaitable_promise final : public awaitable_promise_base {
public:
    using promise_type = awaitable_promise<T>;
    using future_type = awaitable_future<T>;

    awaitable_promise() noexcept = default;
    awaitable_promise(const awaitable_promise&) = delete;
    awaitable_promise& operator=(const awaitable_promise&) = delete;
    awaitable_promise(awaitable_promise&&) = default;
    awaitable_promise& operator=(awaitable_promise&&) = default;

    future_type get_future() {
        return future_type{this};
    }

    // for coroutine
    future_type get_return_object() noexcept {
        return future_type{this};
    };

    // for coroutine
    template <typename Value>
    void return_value(Value&& value) {
        set_value(std::forward<Value>(value));
    }

    template <typename Value>
    void set_value(Value&& value) {
        std::call_once(state_->flag(), [this, value = std::forward<Value>(value)]() {
            result_ = std::move(value);
            state_->ready(true);
            handle_.resume();
        });
    }

    T get() {
        if (!state_->ready()) {
            exception_ = std::make_exception_ptr(invoke_exception{"no value"});
        }
        rethrow_exception();
        return std::move(result_);
    }

private:
    T result_;
};

template <>
class awaitable_promise<void> final : public awaitable_promise_base {
public:
    using promise_type = awaitable_promise<void>;
    using future_type = awaitable_future<void>;

    awaitable_promise() noexcept = default;
    awaitable_promise(const awaitable_promise&) = delete;
    awaitable_promise& operator=(const awaitable_promise&) = delete;
    awaitable_promise(awaitable_promise&&) = default;
    awaitable_promise& operator=(awaitable_promise&&) = default;

    future_type get_future() {
        return future_type{this};
    }

    // for coroutine
    future_type get_return_object() noexcept {
        return future_type{this};
    };

    // for coroutine
    void return_void() {
    }

    void set_value() {
        std::call_once(state_->flag(), [this]() {
            state_->ready(true);
            handle_.resume();
        });
    }

    void get() {
        if (!state_->ready()) {
            exception_ = std::make_exception_ptr(invoke_exception{"no value"});
        }
        rethrow_exception();
    }
};

struct use_awaitable_t final {
    template <typename Result>
    using promise_type = awaitable_promise<Result>;

    template <typename Result>
    using future_type = awaitable_future<Result>;
};

template <typename R, typename... Results>
struct callback_adapter<use_awaitable_t, R(Results...)> final : callback_adapter_impl<use_awaitable_t, Results...> {};

} // namespace detail

constexpr detail::use_awaitable_t use_awaitable{};

template <typename T>
using awaitable_t = awaitable_future<T>;

using awaitable = awaitable_t<void>;

} // namespace rpc
} // namespace cue

#endif // defined(ENABLE_CO_AWAIT)

#endif // CUERPC_USE_AWAITABLE_HPP_
