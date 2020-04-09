# cuerpc

[![Build Status](https://travis-ci.org/xcyl/cuerpc.svg?branch=master)](https://travis-ci.org/xcyl/cuerpc)
[![Codacy Badge](https://api.codacy.com/project/badge/Grade/6960229a428e4e4ba00ae7e7690c5da3)](https://www.codacy.com/manual/xcyl/cuerpc?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=xcyl/cuerpc&amp;utm_campaign=Badge_Grade)
[![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/xcyl/cuerpc.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/xcyl/cuerpc/context:cpp)
[![language](https://img.shields.io/badge/language-C++14-red.svg)](https://en.wikipedia.org/wiki/C++14)
[![GitHub license](https://img.shields.io/badge/license-Apache2.0-blue.svg)](https://raw.githubusercontent.com/xcyl/cuerpc/master/LICENSE)

## 简介

cuerpc是一个使用Modern C++(C++14)编写的RPC框架。cuerpc基于boost.asio，使用[msgpack](https://msgpack.org/)进行序列化/反序列化。提供server与client。

## 使用

cuerpc依赖boost，以及使用最低依赖C++14。cuerpc是header-only的，`#include <cuerpc_server.hpp>`和`#include <cuerpc_client.hpp>`即可使用。

## 示例

### server

```c++
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
```

### client

```c++
#include <iostream>

#include <cuerpc_client.hpp>

using namespace cue::rpc;

struct test_add1 final {
    void add(error_code code, int&& result) {
        std::cout << "invoke test_add1 result " << result << std::endl;
    }
};

struct test_add2 final {
    void add(error_code code, const int& result) {
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
    std::thread run_thread{[&]() {
        while (!c.stopped()) {
            c.run_one();
        }

        // or
        // c.run();
    }};

    if (c.ready(10000)) {
        std::cout << "client ready" << std::endl;

        c.async_invoke("add", make_method<int>(1, 2),
                       [](error_code code, int result) { std::cout << "invoke add1 result " << result << std::endl; });

        c.async_invoke("add", make_method<int>(1, 2), foo2);

        test_add1 ta1;
        c.async_invoke("add", make_method<int>(1, 2), &test_add1::add, &ta1);
        c.async_invoke("add", make_method<int>(1, 2), &test_add1::add);

        register_method<int(int, int)> add2{"add"};
        c.async_invoke(add2(3, 4), [](error_code code, int&& result) {
            std::cout << "invoke add2 result " << result << std::endl;
        });

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

#if defined(ENABLE_CO_AWAIT)
        [&c]() -> awaitable {
            auto r7 = co_await c.async_invoke("add", make_method<int>(5, 0), use_awaitable);
            std::cout << "invoke add7 result " << r7 << std::endl;
        }();

        [&c]() -> awaitable {
            register_method<int(int, int)> add8{"add"};
            auto r8 = co_await c.async_invoke(add8(5, 100), use_awaitable);
            std::cout << "invoke add8 result " << r8 << std::endl;
        }();
#endif // defined(ENABLE_CO_AWAIT)

        c.invoke_oneway("add", make_method<void>(13, 14));

        register_method<void(int, int)> add8{"add"};
        c.invoke_oneway(add8(15, 16));

        c.async_invoke("echo", make_method<std::string>("invoke echo"), [](error_code code, std::string&& result) {
            std::cout << "invoke echo result " << result << std::endl;
        });

        c.async_invoke("good", make_method<std::string>(test_struct{0, "good"}),
                       [](error_code code, const std::string& result) {
                           std::cout << "invoke good result " << result << std::endl;
                       });

        c.async_invoke("great", make_method<test_struct>("great"), [](error_code code, const test_struct& result) {
            std::cout << "invoke great result " << result.msg << std::endl;
        });

        std::thread stop_thread{[&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds{5000});
            c.stop();
        }};

        stop_thread.join();
    }
    run_thread.join();

    std::cout << "end" << std::endl;

    return 0;
}
```

## API

### cue::rpc::server

#### cue::rpc::server& serve(const std::string&/std::string&& name, ...)

注册处理函数。支持多种调用体，可根据实际需求配置回调体调用参数类型。

例：

```c++
s.serve("add", [](int l, int r) { return l + r; });
s.serve("echo", [](const std::string& echo) { return echo; });
s.serve("good", [](test_struct&& test) { return std::move(test.msg); });
s.serve("great", [](std::string msg) { return test_struct{1, std::move(msg)}; });
```

#### cue::rpc::server& listen(unsigned port, [const std::string&/std::string&& host])

监听端口，host为可选的。

#### void run()

运行服务。

### cue::rpc::client

#### client(const std::string&/std::string&& host, unsigned short port)

创建客户端，指定ip与端口。

#### bool ready([std::uint32_t milliseconds])

判断客户端是否已准备就绪，可传入等待超时时间，默认为0，持续等待。

#### void run()

阻塞运行，直至调用停止接口。

#### void run_one()

每运行一次异步事件返回一次。

#### bool stopped() const

客户端是否停止。

#### void stop()

停止客户端运行。

#### auto invoke<[timeout]>(const std::string&/std::string&& name, method<...>&& method)

同步远程调用，timeout为超时时间，可选，默认为0，持续等待。

例：

```c++
auto result = c.invoke("add", make_method<int>(9, 10));
```

#### auto invoke<[timeout]>(method<...>&& method)

同上invoke。

例：

```c++
register_method<int(int, int)> add{"add"};
auto result = c.invoke(add(11, 12));
```

#### auto async_invoke<[timeout]>(const std::string&/std::string&& name, method<...>&& method, Func&& func)

Func分为几种情况。

-   若Func为调用体，则auto为void，调用体的参数类型第一个为错误代码类型，第二个为所需结果类型。若结果类型void，则无第二个类型。

```c++
c.async_invoke("echo", make_method<void>(1, 2), [](error_code code) {
    std::cout << "invoke echo" << std::endl;
});

c.async_invoke("add", make_method<int>(1, 2), [](error_code code, int result) {
    std::cout << "invoke add result " << result << std::endl;
});
```

-   若Func为use_std_future，则auto为std::future\<T>。

```c++
auto r1 = c.async_invoke("add", make_method<int>(5, 6), use_std_future);
std::cout << "invoke add result " << r1.get() << std::endl;

auto r2 = c.async_invoke("add", make_method<void>(5, 6), use_std_future);
r2.get();
```

-   若Func为use_awaitable，则auto是具体结果类型。`使用use_awaitable需要C++20支持，并开启ENABLE_CO_AWAIT宏`。

```c++
auto r1 = co_await c.async_invoke("add", make_method<int>(5, 0), use_awaitable);
std::cout << "invoke add result " << r1 << std::endl;

co_await c.async_invoke("add", make_method<void>(5, 0), use_awaitable);
```

#### auto async_invoke<[timeout]>(method<...>&& method, Func&& func)

同上。

#### method<T, ...> make_method\<T>(...)

创建method。

#### register_method<T(...)>

functor，operator()返回method<T, ...>。
