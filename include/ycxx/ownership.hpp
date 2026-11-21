#pragma once

namespace ycxx::ownership {

// Policy fragments are ordinary types. A namespace-scope alias of use_policy<...> activates them.
template<class Resource> struct affine {};
template<class Resource> struct linear {};

template<class Signature, Signature Function> struct creates {};
template<class Signature, Signature Function> struct observes {};
template<class Signature, Signature Function> struct consumes {};
template<class Signature, Signature Function> struct transfers {};

template<class... Entries> struct policy {};
template<class... Policies> struct use_policy {};

}  // namespace ycxx::ownership
