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

#ifndef CUERPC_COMMON_HPP_
#define CUERPC_COMMON_HPP_

#include <string>
#include <algorithm>
#include <functional>
#include <memory>
#include <sstream>
#include <tuple>
#include <boost/asio.hpp>

#include "cuerpc/detail/noncopyable.hpp"
#include "cuerpc/detail/protocol.hpp"
#include "cuerpc/msgpack.hpp"

namespace cue {
namespace rpc {

class context;

#define SCOPE_BLOCK

using detail::error_code;

namespace detail {

inline const char* code_to_msg(error_code code) noexcept;

} // namespace detail

class invoke_exception final : public std::runtime_error {
public:
    explicit invoke_exception(error_code code) noexcept : std::runtime_error{detail::code_to_msg(code)}, code_{code} {
    }

    template <typename Msg, typename = std::enable_if_t<!std::is_same<Msg, invoke_exception>{}>>
    explicit invoke_exception(Msg&& msg) noexcept : std::runtime_error{std::forward<Msg>(msg)}, code_{} {
    }

    error_code code() const noexcept {
        return code_;
    }

private:
    error_code code_;
};

namespace detail {

// types
using tcp_socket = boost::asio::ip::tcp::socket;

struct request final {
    request_header header;
    std::string payload;
};

struct response final {
    response_header header;
    std::string payload;
};

// global variables
struct global_value final : safe_noncopyable {
    // for empty string const reference
    static const std::string& empty_string() noexcept {
        static const std::string empty{""};
        return empty;
    }
};

// meta utilities
template <typename...>
using void_t = void;

template <typename T, typename = void>
struct has_operator : std::false_type {};

template <typename T>
struct has_operator<T, void_t<decltype(&T::operator())>> : std::true_type {};

template <typename T, typename = void>
struct is_functor : std::false_type {};

template <typename T>
struct is_functor<T, void_t<std::enable_if_t<!std::is_function<T>{} && has_operator<T>{}>>> : std::true_type {};

template <typename T>
struct to_function;

template <typename T, typename R, typename... Args>
struct to_function<R (T::*)(Args...) const> {
    using type = std::function<R(Args...)>;
};

template <typename T, typename R, typename... Args>
struct to_function<R (T::*)(Args...)> {
    using type = std::function<R(Args...)>;
};

template <typename T>
using to_function_t = typename to_function<T>::type;

template <typename T>
struct function_args;

template <typename R, typename... Args>
struct function_args<R(Args...)> {
    using return_type = R;
    static constexpr std::size_t arity{sizeof...(Args)};

    template <std::size_t N>
    struct arg {
        using type = typename std::tuple_element<N, std::tuple<Args...>>::type;
    };

    template <std::size_t N>
    using arg_t = typename arg<N>::type;
};

template <typename R>
struct function_args<R()> {
    using return_type = R;
    static constexpr std::size_t arity{0};

    template <std::size_t N>
    struct arg {
        using type = void;
    };

    template <std::size_t N>
    using arg_t = typename arg<N>::type;
};

template <typename R, typename... Args>
struct function_args<R (*)(Args...)> : function_args<R(Args...)> {};

template <typename R, typename... Args>
struct function_args<std::function<R(Args...)>> : function_args<R(Args...)> {};

template <typename R, typename T, typename... Args>
struct function_args<R (T::*)(Args...)> : function_args<R(Args...)> {};

template <typename R, typename T, typename... Args>
struct function_args<R (T::*)(Args...) const> : function_args<R(Args...)> {};

template <typename T>
struct function_args : function_args<decltype(&T::operator())> {};

template <typename T, typename = void>
struct is_void_result : std::false_type {};

template <typename Func>
struct is_void_result<
    Func,
    void_t<std::enable_if_t<function_args<std::decay_t<Func>>::arity == 1 &&
                            std::is_same<error_code, typename function_args<std::decay_t<Func>>::template arg_t<0>>{}>>>
    : std::true_type {};

template <typename R, typename Func, typename = void>
struct is_not_void_result : std::false_type {};

template <typename R, typename Func>
struct is_not_void_result<
    R, Func,
    void_t<std::enable_if_t<
        function_args<std::decay_t<Func>>::arity == 2 &&
        std::is_same<error_code, typename function_args<std::decay_t<Func>>::template arg_t<0>>{} &&
        std::is_same<R, std::decay_t<typename function_args<std::decay_t<Func>>::template arg_t<1>>>{}>>>
    : std::true_type {};

// utilities functions
struct utils final : safe_noncopyable {
    inline static bool iequals(const std::string& lhs, const std::string& rhs) noexcept {
        return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                          [](char l, char r) { return std::tolower(l) == std::tolower(r); });
    }

    inline static std::string to_lower(const std::string& str) noexcept {
        std::string lower_str;
        for (const auto& c : str) {
            lower_str += std::tolower(c);
        }
        return lower_str;
    }
};

inline const char* code_to_msg(error_code code) noexcept {
    switch (code) {
    case error_code::success:
        return "invoke empty result";
    case error_code::error:
        return "invoke error";
    case error_code::exception:
        return "invoke exception";
    case error_code::timeout:
        return "invoke timeout";
    case error_code::nonsupport:
        return "invoke nonsupport";
    default:
        return "invoke unknown exception";
    }
}

// utilities classes

} // namespace detail
} // namespace rpc
} // namespace cue

#endif // CUERPC_COMMON_HPP_
