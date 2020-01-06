#ifndef CUERPC_STUB_HPP_
#define CUERPC_STUB_HPP_

#include <sstream>
#include <exception>

#include "cuerpc/msgpack.hpp"
#include "cuerpc/detail/noncopyable.hpp"

namespace cue {
namespace rpc {
namespace detail {

struct stub final : safe_noncopyable {
    template <typename T>
    inline static T unpack(const std::string& payload) {
        return msgpack::unpack(payload.data(), payload.size()).get().as<T>();
    }

    template <typename... Args>
    inline static std::string pack(Args&&... args) {
        std::stringstream ss;
        msgpack::pack(ss, std::forward_as_tuple(std::forward<Args>(args)...));
        return ss.str();
    }
};

} // namespace detail
} // namespace rpc
} // namespace cue

#endif // CUERPC_STUB_HPP_
