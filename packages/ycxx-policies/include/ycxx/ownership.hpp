#pragma once

namespace ycxx::ownership {

// Constant values for C++26 declaration annotations.
enum class resource_mode { affine = 1, linear = 2 };
enum class operation { creates = 1, observes, consumes, transfers, verified_observes };
enum class move_constructor { transfers = 1 };

// Policy fragments are ordinary types. A namespace-scope alias of use_policy<...> activates them.
template<class Resource> struct affine {};
template<class Resource> struct linear {};
template<class Resource> struct move_transfers {};

template<class Signature, Signature Function> struct creates {};
template<class Signature, Signature Function> struct observes {};
template<class Signature, Signature Function> struct verified_observes {};
template<class Signature, Signature Function> struct consumes {};
template<class Signature, Signature Function> struct transfers {};

template<class... Entries> struct policy {};
template<class... Policies> struct use_policy {};

}  // namespace ycxx::ownership
