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

#ifndef CUERPC_DISPATCHER_HPP_
#define CUERPC_DISPATCHER_HPP_

#include <functional>
#include <unordered_map>
#include <tuple>

#include "cuerpc/detail/noncopyable.hpp"
#include "cuerpc/detail/session.hpp"
#include "cuerpc/detail/protocol.hpp"
#include "cuerpc/detail/stub.hpp"

namespace cue {
namespace rpc {
namespace detail {

class dispatcher final : safe_noncopyable {
public:
    static dispatcher& instance() noexcept {
        static dispatcher dispatcher;
        return dispatcher;
    }

    template <typename _Name, typename _Func>
    void serve(_Name&& name, _Func&& func) {
        add_func(std::forward<_Name>(name), std::forward<_Func>(func));
    }

    template <typename _Name, typename _Ty, typename _Func, typename _Self>
    void serve(_Name&& name, _Func _Ty::*func, _Self self) {
        add_func(std::forward<_Name>(name), func, self);
    }

    template <typename _Name, typename _Ty, typename _Func>
    void serve(_Name&& name, _Func _Ty::*func) {
        add_func(std::forward<_Name>(name), func, static_cast<_Ty*>(nullptr));
    }

    void dispatch(std::shared_ptr<session> session, std::shared_ptr<request> req) {
        std::string invoke_name;
        try {
            invoke_name = std::get<0>(stub::unpack<std::tuple<std::string>>(req->payload));
        } catch (...) {
            reply(session, req, "", error_code::exception);
            return;
        }
        const auto it = invokes_.find(invoke_name);
        if (it != invokes_.end()) {
            return it->second(session, req);
        }

        // nonsupport
        reply(session, req, "", error_code::nonsupport);
    }

private:
    dispatcher() noexcept = default;

    template <typename _Name, typename _Func>
    void add_func(_Name&& name, _Func&& func) {
        invokes_.emplace(
            std::forward<_Name>(name),
            [this, func = std::forward<_Func>(func)](std::shared_ptr<session> session, std::shared_ptr<request> req) {
                this->invoke(std::move(func), session, req, is_functor<_Func>{});
            });
    }

    template <typename _Name, typename _Ty, typename _Func, typename _Self>
    void add_func(_Name&& name, _Func _Ty::*func, _Self self) {
        invokes_.emplace(std::forward<_Name>(name),
                         [this, func, self](std::shared_ptr<session> session, std::shared_ptr<request> req) {
                             this->invoke(func, self, session, req);
                         });
    }

    template <typename _Func>
    inline static void invoke(_Func func, std::shared_ptr<session> session, std::shared_ptr<request> req,
                              std::false_type) {
        invoke_proxy(func, session, req);
    }

    template <typename _Func, typename = void_t<decltype(&_Func::operator())>>
    inline static void invoke(_Func func, std::shared_ptr<session> session, std::shared_ptr<request> req,
                              std::true_type) {
        invoke_proxy(to_function_t<decltype(&_Func::operator())>(func), session, req);
    }

    template <typename _Ty, typename _Func, typename _Self>
    inline static void invoke(_Func _Ty::*func, _Self self, std::shared_ptr<session> session,
                              std::shared_ptr<request> req) {
        invoke_proxy(func, self, session, req);
    }

    template <typename _Ret, typename... _Args>
    inline static void invoke_proxy(_Ret (*func)(_Args...), std::shared_ptr<session> session,
                                    std::shared_ptr<request> req) {
        invoke_proxy(std::function<_Ret(_Args...)>(func), session, req);
    }

    template <typename _Ret, typename _Ty, typename _Self, typename... _Args>
    inline static void invoke_proxy(_Ret (_Ty::*func)(_Args...) const, _Self self, std::shared_ptr<session> session,
                                    std::shared_ptr<request> req) {
        invoke_proxy(std::decay_t<decltype(func)>(func), self, session, req);
    }

    template <typename _Ret, typename... _Args>
    inline static void invoke_proxy(std::function<_Ret(_Args...)> func, std::shared_ptr<session> session,
                                    std::shared_ptr<request> req) {
        std::tuple<std::string, std::tuple<std::decay_t<_Args>...>> args_tuple;
        try {
            args_tuple = stub::unpack<std::tuple<std::string, std::tuple<std::decay_t<_Args>...>>>(req->payload);
        } catch (...) {
            reply(session, req, "", error_code::exception);
            return;
        }
        auto payload = apply<_Ret>(std::move(func), std::move(std::get<1>(args_tuple)));
        reply(session, req, std::move(payload));
    }

    template <typename _Ret, typename _Ty, typename _Self, typename... _Args>
    inline static void invoke_proxy(_Ret (_Ty::*func)(_Args...), _Self self, std::shared_ptr<session> session,
                                    std::shared_ptr<request> req) {
        std::tuple<std::string, std::tuple<std::decay_t<_Args>...>> args_tuple;
        try {
            args_tuple = stub::unpack<std::tuple<std::string, std::tuple<std::decay_t<_Args>...>>>(req->payload);
        } catch (...) {
            reply(session, req, "", error_code::exception);
            return;
        }
        auto wrapper = [=](_Args... args) {
            if (self) {
                return (self->*func)(args...);
            } else {
                return (_Ty{}.*func)(args...);
            }
        };
        auto payload = apply<_Ret>(std::move(wrapper), std::move(std::get<1>(args_tuple)));
        reply(session, req, std::move(payload));
    }

    template <typename _Ret, typename _Func, typename _Tuple>
    inline static std::enable_if_t<std::is_void<_Ret>{}, std::string> apply(_Func func, _Tuple&& t) {
        constexpr auto tuple_size = std::tuple_size<std::decay_t<_Tuple>>{};
        apply_proxy(std::move(func), std::forward<_Tuple>(t), std::make_index_sequence<tuple_size>{});
        return std::string{};
    }

    template <typename _Ret, typename _Func, typename _Tuple>
    inline static std::enable_if_t<!std::is_void<_Ret>{}, std::string> apply(_Func func, _Tuple&& t) {
        constexpr auto tuple_size = std::tuple_size<std::decay_t<_Tuple>>{};
        auto result = apply_proxy(std::move(func), std::forward<_Tuple>(t), std::make_index_sequence<tuple_size>{});
        return stub::pack(std::move(result));
    }

    template <typename _Func, typename _Tuple, std::size_t... Indexes>
    inline static decltype(auto) apply_proxy(_Func func, _Tuple&& t, std::index_sequence<Indexes...>) {
        return func(std::get<Indexes>(std::forward<_Tuple>(t))...);
    }

    inline static void reply(std::shared_ptr<session> session, std::shared_ptr<request> req, std::string&& payload,
                             error_code code = error_code::success) {
        if (req->header.type == request_type::oneway) {
            return;
        }

        response_header header;
        header.code = code;
        header.request_id = req->header.request_id;
        header.payload_length = payload.size();
        session->reply({std::move(header), std::move(payload)});
    }

    std::unordered_map<std::string, std::function<void(std::shared_ptr<session>, std::shared_ptr<request>)>> invokes_;
};

} // namespace detail
} // namespace rpc
} // namespace cue

#endif // CUERPC_DISPATCHER_HPP_
