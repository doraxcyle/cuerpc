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

#ifndef CUERPC_USE_FUTURE_HPP_
#define CUERPC_USE_FUTURE_HPP_

#include <tuple>
#include <future>
#include <memory>

#include "cuerpc/detail/common.hpp"

namespace cue {
namespace rpc {
namespace detail {

struct return_void {
    void get() {
    }
};

template <typename Callable, typename Signature>
struct callback_adapter {
    using callback_type = Callable;
    using return_type = return_void;

    inline static std::tuple<callback_type, return_type> traits(Callable&& callback) {
        return {std::move(callback), {}};
    }
};

template <typename Promise, typename Result>
class use_future_handler_base {
public:
    using result_type = Result;
    using promise_type = typename Promise::template promise_type<result_type>;

    use_future_handler_base() noexcept : promise_{std::make_shared<promise_type>()} {
    }

    auto get_future() const {
        return promise_->get_future();
    }

protected:
    std::shared_ptr<promise_type> promise_;
};

template <typename...>
struct use_future_handler;

template <typename Promise>
struct use_future_handler<Promise, error_code> final : use_future_handler_base<Promise, void> {
    void operator()(error_code code) const {
        if (code == error_code::success) {
            this->promise_->set_value();
        } else {
            this->promise_->set_exception(std::make_exception_ptr(invoke_exception{code}));
        }
    }
};

template <typename Promise, typename Result>
struct use_future_handler<Promise, error_code, Result> final : use_future_handler_base<Promise, Result> {
    template <typename Arg>
    void operator()(error_code code, Arg&& arg) const {
        if (code == error_code::success) {
            this->promise_->set_value(std::forward<Arg>(arg));
        } else {
            this->promise_->set_exception(std::make_exception_ptr(invoke_exception{code}));
        }
    }
};

template <typename Future, typename Result>
class use_future_return final {
public:
    using result_type = Result;
    using future_type = typename Future::template future_type<result_type>;

    use_future_return(future_type&& future) noexcept : future_{std::move(future)} {
    }

    future_type get() {
        return std::move(future_);
    }

private:
    future_type future_;
};

template <typename Traits, typename... Results>
struct callback_adapter_impl {
    using traits_type = Traits;
    using callback_type = use_future_handler<traits_type, Results...>;
    using result_type = typename callback_type::result_type;
    using return_type = use_future_return<traits_type, result_type>;

    inline static std::tuple<callback_type, return_type> traits(const Traits&) {
        callback_type callback{};
        auto future = callback.get_future();
        return {std::move(callback), std::move(future)};
    }
};

struct use_std_future_t final {
    template <typename Result>
    using promise_type = std::promise<Result>;

    template <typename Result>
    using future_type = std::future<Result>;
};

template <typename R, typename... Results>
struct callback_adapter<use_std_future_t, R(Results...)> final : callback_adapter_impl<use_std_future_t, Results...> {};

} // namespace detail

constexpr detail::use_std_future_t use_std_future{};

} // namespace rpc
} // namespace cue

#endif // CUERPC_USE_FUTURE_HPP_
