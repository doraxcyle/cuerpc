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

#include <iostream>

#include <cuerpc_client.hpp>

using namespace cue::rpc;

struct test_add1 final {
    void add(error_code code, int result) {
        std::cout << "invoke test_add1 result " << result << std::endl;
    }
};

struct test_add2 final {
    void add(error_code code, int result) {
        std::cout << "invoke test_add2 result " << result << std::endl;
    }
};

void foo1(error_code code) {
}

void foo2(error_code code, int result) {
}

struct test_struct {
    int code;
    std::string msg;

    MSGPACK_DEFINE(code, msg);
};

int main(int argc, char** argv) {
    client c{"127.0.0.1", 10002};
    c.ready();

    std::cout << "client ready" << std::endl;

    c.async_invoke("add", make_method<int>(1, 2),
                   [](error_code code, int result) { std::cout << "invoke add1 result " << result << std::endl; });

    c.async_invoke("add", make_method<int>(1, 2), foo2);

    test_add1 ta1;
    c.async_invoke("add", make_method<int>(1, 2), &test_add1::add, &ta1);
    c.async_invoke("add", make_method<int>(1, 2), &test_add1::add);

    register_method<int(int, int)> add2{"add"};
    c.async_invoke(add2(3, 4),
                   [](error_code code, int result) { std::cout << "invoke add2 result " << result << std::endl; });

    register_method<void(int, int)> call1{"add"};
    c.async_invoke(call1(3, 4), [](error_code code) { std::cout << "invoke add2 result " << std::endl; });

    test_add2 ta2;
    c.async_invoke(add2(3, 4), &test_add2::add, &ta2);
    c.async_invoke(add2(3, 4), &test_add2::add);

    auto r3 = c.async_invoke("add", make_method<int>(5, 6), use_std_future);
    std::cout << "invoke add3 result " << r3.get() << std::endl;

    auto r4 = c.async_invoke("add", make_method<void>(5, 6), use_std_future);
    r4.get();

    auto r5 = c.invoke("add", make_method<int>(9, 10));
    std::cout << "invoke add5 result " << r5 << std::endl;

    register_method<int(int, int)> add6{"add"};
    auto r6 = c.invoke(add6(11, 12));
    std::cout << "invoke add6 result " << r6 << std::endl;

    [&c]() -> awaitable {
        auto r7 = co_await c.async_invoke("add", make_method<int>(5, 0), use_awaitable);
        std::cout << "invoke add7 result " << r7 << std::endl;
    }();

    [&c]() -> awaitable {
        register_method<int(int, int)> add8{"add"};
        auto r8 = co_await c.async_invoke(add8(5, 100), use_awaitable);
        std::cout << "invoke add8 result " << r8 << std::endl;
    }();

    c.invoke_oneway("add", make_method<void>(13, 14));

    register_method<void(int, int)> add8{"add"};
    c.invoke_oneway(add8(15, 16));

    c.async_invoke("echo", make_method<std::string>("invoke echo"), [](error_code code, std::string&& result) {
        std::cout << "invoke echo result " << result << std::endl;
    });

    c.async_invoke(
        "good", make_method<std::string>(test_struct{0, "good"}),
        [](error_code code, const std::string& result) { std::cout << "invoke good result " << result << std::endl; });

    c.async_invoke("great", make_method<test_struct>("great"), [](error_code code, const test_struct& result) {
        std::cout << "invoke great result " << result.msg << std::endl;
    });

    std::cout << "end" << std::endl;

    c.run();

    return 0;
}
