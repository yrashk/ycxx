#pragma once

#include <ycxx/ownership.hpp>

namespace ycxx::effects {

template<class... Tags> struct set {};

// A positive finite upper bound for one tag in a set.
template<class Tag, unsigned long long Maximum> requires (Maximum > 0)
struct at_most {};

// Portable policy entries for a function with a declared upper bound.
template<class Signature, Signature Function, class Effects> struct bound {};
template<class Signature, Signature Function, class Effects> struct trusted {};

// Equivalent C++26 annotations on a function declaration.
template<class Effects> struct contract {};
template<class Effects> struct trusted_contract {};

using ownership::policy;
using ownership::use_policy;

}  // namespace ycxx::effects
