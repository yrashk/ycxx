# YCXX Policies

YCXX Policies is a standalone C++20 header library with ownership guards and scoped capability
tokens. GCC and Clang enforce the ordinary C++ type rules and execute the guards. YCXX can
additionally verify declared ownership policies when `-fcheck-policies` is enabled.

## Use with GCC or Clang

Configure and install this package directory with the compiler used by your project.

```sh
cmake -S packages/ycxx-policies -B build/ycxx-policies \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_INSTALL_PREFIX="$PWD/stage"
cmake --install build/ycxx-policies
```

Tell CMake where to find the package with `-DCMAKE_PREFIX_PATH=/path/to/stage`, then link the
interface target to your application.

```cmake
find_package(YCXXPolicies CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE YCXXPolicies::policies)
```

To vendor the source, copy this directory into your project and use
`add_subdirectory(vendor/ycxx-policies)`. Link the same interface target. A non-CMake build can
add this directory's `include` path to the compiler's include search path.

## Ownership adapters

`<ycxx/ownership/runtime.hpp>` provides `ycxx::ownership::affine_state` and
`ycxx::ownership::linear_state`. Store a state in the adapter that owns the resource. A move
transfers the live state and invalidates the source. Call `observe()` before accessing a live
resource and `consume()` when the adapter discharges it. Destruction of a live linear state or
an operation on a spent state terminates execution. The adapter owns the actual resource and
implements its cleanup. YCXX, GCC, and Clang use the same trap intrinsic for violations so
their inline guard definitions agree when mixed in one executable. The guard uses
`std::abort()` on compilers without that intrinsic.

```cpp
#include <ycxx/ownership/runtime.hpp>

int main() {
  ycxx::ownership::linear_state pending;
  auto next = static_cast<ycxx::ownership::linear_state&&>(pending);
  next.consume();
}
```

An API can publish metadata in `<ycxx/ownership.hpp>` as reusable `policy<...>` types. Consumers
activate several fragments with a namespace-scope alias of `use_policy<...>`. The metadata has
no runtime work. YCXX interprets it when ownership checking is enabled. Use typed template
arguments for portability and C++26 declaration annotations with compiler support.

An observation marked `verified_observes<decltype(&inspect), &inspect>` asks YCXX to
verify the observer's body. Supported bodies can copy scalar fields and update independent
scalar storage. For separate builds, include the contract in the caller and provider and pass
`-fcheck-policies` when compiling each object and at the final YCXX link. The checked link
supports explicit objects and direct calls to unique strong definitions. GCC and Clang accept
the metadata as ordinary C++20 types.

## Scoped capabilities

Include `<ycxx/capabilities.hpp>` and choose an issuer type, a bit for each right, and stable
resource identifiers within the issuer's domain. Only the issuer can construct
`token<Issuer, Granted>` from a scope identifier. Its grants should come from your application's
authorization logic. Use identifiers that cannot be reassigned while a token for the old
resource is live. Instances of the same issuer type share the scope identifier space.

Use `grants<Granted, Required>` to constrain an adapter's interface. Call
`authority.template require<Required>(scope)` before accessing the resource. That operation
requires the rights at compile time and checks the scope and the token's live state at runtime.

```cpp
#include <ycxx/capabilities.hpp>

namespace caps = ycxx::capabilities;

class Store {
  static constexpr caps::rights_mask write_right = 2;
  inline static int cells_[2]{};

  // Issuance belongs to the application's trusted authorization path.
  static caps::token<Store, write_right> issue_primary_writer() noexcept {
    return {1};
  }

 public:
  template<caps::rights_mask Granted> requires caps::grants<Granted, write_right>
  static void write(const caps::token<Store, Granted>& authority, caps::scope_id scope, int value) noexcept {
    authority.template require<write_right>(scope);
    cells_[scope - 1] = value;
  }
};
```

Tokens are move-only. Moving invalidates the source. Calling `release()` invalidates the
held token before it goes out of scope. Operation guards terminate on a wrong scope, a
moved-from token, or a released token. GCC and Clang support this API. Native YCXX instantiation
depends on dependent type friendship. [Compiler prerequisite #689](https://github.com/yrashk/ycxx/issues/689)
