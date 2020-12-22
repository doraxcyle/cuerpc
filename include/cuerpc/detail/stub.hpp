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
    template <typename _Ty>
    inline static _Ty unpack(std::string_view payload) {
        return msgpack::unpack(payload.data(), payload.size()).get().as<_Ty>();
    }

    template <typename... _Args>
    inline static std::string pack(_Args&&... args) {
        std::stringstream ss;
        msgpack::pack(ss, std::forward_as_tuple(std::forward<_Args>(args)...));
        return ss.str();
    }
};

} // namespace detail
} // namespace rpc
} // namespace cue

#endif // CUERPC_STUB_HPP_
