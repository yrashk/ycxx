# YCXX Policies

YCXX Policies is a standalone C++20 header library for resource ownership, capabilities, and
effects. The ownership component is available today. GCC and Clang enforce the ordinary C++ type
rules and run the same adapter guards used by YCXX. YCXX can additionally verify declared
ownership policies when `-fcheck-policies` is enabled.

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
