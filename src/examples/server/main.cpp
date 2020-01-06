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

#include <cuerpc_server.hpp>

using namespace cue::rpc;

int f0() {
    std::cout << "f0" << std::endl;
    return 0;
}

void f1() {
    std::cout << "f1" << std::endl;
}

void f2(int a) {
    std::cout << "f2 a: " << a << std::endl;
}

int f3(int a, int b) {
    std::cout << "f3 a: " << a << " b: " << b << std::endl;
    return a + b;
}

int f4(int a, int b) {
    std::cout << "f4 a: " << a << " b: " << b << std::endl;
    return a + b;
}

struct test1 {
    void f6() {
        std::cout << "f6" << std::endl;
    }
};

struct test2 {
    void f7() const {
        std::cout << "f7" << std::endl;
    }
};

struct test3 {
    void operator()(int a) {
        std::cout << "f8" << std::endl;
    }
};

struct test4 {
    void operator()(int a) const {
        std::cout << "f9" << std::endl;
    }
};

struct test_struct {
    int code;
    std::string msg;

    MSGPACK_DEFINE(code, msg);
};

int main(int argc, char** argv) {
    server s;

    s.serve("f0", f0);
    s.serve("f1", f1);
    s.serve("f2", f2);
    s.serve("f3", f3);
    s.serve("f4", std::function<void(int, int)>(f4));
    s.serve("f5", [](int a) { std::cout << "f5 a: " << a << std::endl; });
    s.serve("f6", &test1::f6);
    test2 t2;
    s.serve("f7", &test2::f7, &t2);
    s.serve("f8", test3{});
    s.serve("f9", test4{});

    s.serve("add", [](int l, int r) { return l + r; });

    s.serve("echo", [](const std::string& echo) { return echo; });

    s.serve("good", [](test_struct&& test) { return std::move(test.msg); });

    s.serve("great", [](std::string msg) { return test_struct{1, std::move(msg)}; });

    s.listen(10002);

    s.run();

    return 0;
}
