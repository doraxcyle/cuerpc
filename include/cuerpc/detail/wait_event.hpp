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

#ifndef CUERPC_WAIT_EVENT_H_
#define CUERPC_WAIT_EVENT_H_

#include <mutex>
#include <condition_variable>
#include <atomic>

#include "cuerpc/detail/noncopyable.hpp"

namespace cue {
namespace rpc {
namespace detail {

class wait_event final : safe_noncopyable {
public:
    wait_event() noexcept = default;
    ~wait_event() noexcept = default;

    void wait() {
        std::unique_lock<std::mutex> lock{stop_mutex_};
        stop_wait_.wait(lock, [this]() { return stop_.load(); });
    }

    bool wait_for(long milliseconds) {
        std::unique_lock<std::mutex> lock{stop_mutex_};
        return stop_wait_.wait_for(lock, std::chrono::milliseconds{milliseconds}, [this]() { return stop_.load(); });
    }

    void stop() {
        stop_ = true;
        stop_wait_.notify_all();
    }

private:
    std::atomic_bool stop_{false};
    std::mutex stop_mutex_;
    std::condition_variable stop_wait_;
};

} // namespace detail
} // namespace rpc
} // namespace cue

#endif // CUERPC_WAIT_EVENT_H_
