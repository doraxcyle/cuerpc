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
#if __cplusplus > 201402L
#include <string_view>
#else
#include "cuerpc/deps/string_view.hpp"
namespace std {
using namespace nonstd;
namespace literals {
using namespace nonstd::literals::string_view_literals;
namespace string_view_literals {
using namespace nonstd::literals::string_view_literals;
} // namespace string_view_literals
} // namespace literals
namespace string_view_literals {
using namespace nonstd::literals::string_view_literals;
} // namespace string_view_literals
} // namespace std
#endif // __cplusplus > 201402L
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

inline std::string_view code_to_msg(error_code code) noexcept;

} // namespace detail

class invoke_exception final : public std::runtime_error {
public:
    explicit invoke_exception(error_code code) noexcept
        : std::runtime_error{detail::code_to_msg(code).data()}, code_{code} {
    }

    template <typename _Msg, typename = std::enable_if_t<!std::is_same<_Msg, invoke_exception>{}>>
    explicit invoke_exception(_Msg&& msg) noexcept : std::runtime_error{std::forward<_Msg>(msg)}, code_{} {
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

// meta utilities
template <typename...>
using void_t = void;

template <typename _Ty, typename = void>
struct has_operator : std::false_type {};

template <typename _Ty>
struct has_operator<_Ty, void_t<decltype(&_Ty::operator())>> : std::true_type {};

template <typename _Ty, typename = void>
struct is_functor : std::false_type {};

template <typename _Ty>
struct is_functor<_Ty, void_t<std::enable_if_t<!std::is_function<_Ty>{} && has_operator<_Ty>{}>>> : std::true_type {};

template <typename _Ty>
struct to_function;

template <typename _Ty, typename _Ret, typename... _Args>
struct to_function<_Ret (_Ty::*)(_Args...) const> {
    using type = std::function<_Ret(_Args...)>;
};

template <typename _Ty, typename _Ret, typename... _Args>
struct to_function<_Ret (_Ty::*)(_Args...)> {
    using type = std::function<_Ret(_Args...)>;
};

template <typename _Ty>
using to_function_t = typename to_function<_Ty>::type;

template <typename _Ty>
struct function_args;

template <typename _Ret, typename... _Args>
struct function_args<_Ret(_Args...)> {
    using return_type = _Ret;
    static constexpr std::size_t arity{sizeof...(_Args)};

    template <std::size_t _Index>
    struct arg {
        using type = typename std::tuple_element<_Index, std::tuple<_Args...>>::type;
    };

    template <std::size_t _Index>
    using arg_t = typename arg<_Index>::type;
};

template <typename _Ret>
struct function_args<_Ret()> {
    using return_type = _Ret;
    static constexpr std::size_t arity{0};

    template <std::size_t _Index>
    struct arg {
        using type = void;
    };

    template <std::size_t _Index>
    using arg_t = typename arg<_Index>::type;
};

template <typename _Ret, typename... _Args>
struct function_args<_Ret (*)(_Args...)> : function_args<_Ret(_Args...)> {};

template <typename _Ret, typename... _Args>
struct function_args<std::function<_Ret(_Args...)>> : function_args<_Ret(_Args...)> {};

template <typename _Ret, typename _Ty, typename... _Args>
struct function_args<_Ret (_Ty::*)(_Args...)> : function_args<_Ret(_Args...)> {};

template <typename _Ret, typename _Ty, typename... _Args>
struct function_args<_Ret (_Ty::*)(_Args...) const> : function_args<_Ret(_Args...)> {};

template <typename _Ty>
struct function_args : function_args<decltype(&_Ty::operator())> {};

template <typename _Ty, typename = void>
struct is_void_result : std::false_type {};

template <typename _Func>
struct is_void_result<_Func,
                      void_t<std::enable_if_t<
                          function_args<std::decay_t<_Func>>::arity == 1 &&
                          std::is_same<error_code, typename function_args<std::decay_t<_Func>>::template arg_t<0>>{}>>>
    : std::true_type {};

template <typename _Ret, typename _Func, typename = void>
struct is_not_void_result : std::false_type {};

template <typename _Ret, typename _Func>
struct is_not_void_result<
    _Ret, _Func,
    void_t<std::enable_if_t<
        function_args<std::decay_t<_Func>>::arity == 2 &&
        std::is_same<error_code, typename function_args<std::decay_t<_Func>>::template arg_t<0>>{} &&
        std::is_same<_Ret, std::decay_t<typename function_args<std::decay_t<_Func>>::template arg_t<1>>>{}>>>
    : std::true_type {};

// utilities functions
struct utils final : safe_noncopyable {
    static bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
        return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                          [](char l, char r) { return std::tolower(l) == std::tolower(r); });
    }

    static std::string to_lower(std::string_view str) noexcept {
        std::string lower_str;
        for (const auto& c : str) {
            lower_str += std::tolower(c);
        }
        return lower_str;
    }
};

inline std::string_view code_to_msg(error_code code) noexcept {
    using namespace std::literals;
    switch (code) {
    case error_code::success:
        return "invoke empty result"sv;
    case error_code::error:
        return "invoke error"sv;
    case error_code::exception:
        return "invoke exception"sv;
    case error_code::timeout:
        return "invoke timeout"sv;
    case error_code::nonsupport:
        return "invoke nonsupport"sv;
    default:
        return "invoke unknown exception"sv;
    }
}

// utilities classes

} // namespace detail
} // namespace rpc
} // namespace cue

#endif // CUERPC_COMMON_HPP_
