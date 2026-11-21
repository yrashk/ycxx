#pragma once

#include "../../include/ycxx/ownership.hpp"

namespace ycxx_affine {

class Permit {
  int token_;
  Permit() : token_(0) {}
  // Keep copying private while using the ordinary trivial-copy calling convention.
  Permit(const Permit&) = default;
  friend Permit issue() noexcept;

 public:
  Permit& operator=(const Permit&) = delete;
};

inline Permit issue() noexcept { return Permit{}; }
inline void inspect(const Permit&) noexcept {}
inline void redeem(Permit&&) noexcept {}

using PermitPolicy = ycxx::ownership::policy<
    ycxx::ownership::affine<Permit>,
    ycxx::ownership::creates<decltype(&issue), &issue>,
    ycxx::ownership::observes<decltype(&inspect), &inspect>,
    ycxx::ownership::consumes<decltype(&redeem), &redeem>>;
using CheckPermit = ycxx::ownership::use_policy<PermitPolicy>;

}  // namespace ycxx_affine
