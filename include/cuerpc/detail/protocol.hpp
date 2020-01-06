
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

// cuerpc protocol
//
// request packet
// 0     1     2     3     4                                              12                      16
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+
// |match| ver | type|codec|                   request id                  |        timeout        |
// +-----------+-----------+-----------+-----------+-----------+-----------+-----------+-----------+
// |     payload length    |                                                                       |
// +-----------+-----------+                                                                       +
// |                                         payload                                               |
// +                                                                                               +
// |                                         ... ...                                               |
// +-----------------------------------------------------------------------------------------------+
//
// response packet
// 0     1     2     3     4                                              12    13                16
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+-----+
// |match| ver | type|codec|                   request id                  | code|  payload length |
// +-----------+-----------+-----------+-----------+-----------+-----------+-----------+-----------+
// |     |                                                                                         |
// +-----+                                                                                         +
// |                                         payload                                               |
// +                                                                                               +
// |                                         ... ...                                               |
// +-----------------------------------------------------------------------------------------------+

#ifndef CUERPC_PROTOCOL_HPP_
#define CUERPC_PROTOCOL_HPP_

#include <cstdint>

namespace cue {
namespace rpc {
namespace detail {

constexpr char protocol_match{'@'};

enum class request_type : uint8_t { heartbeat = 0, request, response, oneway };

enum class codec_type : uint8_t { msgpack = 0 };

enum class code_type : uint8_t { success = 0, error, exception, timeout, nonsupport };

#pragma pack(1)
struct request_header final {
    uint8_t version{1};
    request_type type;
    codec_type codec{codec_type::msgpack};
    uint64_t request_id;
    uint32_t timeout;
    uint32_t payload_length;
};

struct response_header final {
    uint8_t version{1};
    request_type type{request_type::response};
    codec_type codec{codec_type::msgpack};
    uint64_t request_id;
    code_type code{code_type::success};
    uint32_t payload_length{0};
};
#pragma pack()

} // namespace detail
} // namespace rpc
} // namespace cue

#endif // CUERPC_PROTOCOL_HPP_
