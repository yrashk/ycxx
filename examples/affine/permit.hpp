#pragma once

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

}  // namespace ycxx_affine
