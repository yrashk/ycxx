# YCXX Policies

YCXX Policies is a standalone C++20 header library with ownership guards, scoped capability
tokens, effect budgets, and handlers. GCC and Clang enforce the ordinary C++ type rules and
execute the guards. YCXX also verifies declared ownership and effect policies with `-fcheck-policies`.

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

On native x86-64 Linux and arm64 macOS, a hosted C++ caller can pass a guard to a YCXX-built
helper. A single nonthrowing local destructor in the helper can consume the guard after a direct
external call returns or throws. When the call throws, its original exception reaches the hosted
caller. [Mixed-compiler fixture](../../tests/unit/pipeline/policies_contracts.py)

An API can publish metadata in `<ycxx/ownership.hpp>` as reusable `policy<...>` types. Consumers
activate several fragments with a namespace-scope alias of `use_policy<...>`. The metadata has
no runtime work. YCXX interprets it when ownership checking is enabled. Use typed template
arguments for portability and C++26 declaration annotations with compiler support.
Guard the reflection attributes with `#ifdef __ycxx__` to build the same source as C++20
with GCC and Clang while retaining the runtime guards.

Add `move_transfers<Resource>` as a separate policy entry when the resource's
`Resource(Resource&&) noexcept` constructor moves a live obligation and invalidates its source.
With that declaration, YCXX checks that the named source is live and unborrowed. A plain
wrapper with a nonthrowing, move-only guard can use its implicit move constructor to transfer
the obligation alongside its scalar fields. Use `[[=ycxx::ownership::move_constructor::transfers]]`
on the resource's class declaration for the equivalent C++26 annotation.

Save this example as `receipt.cpp`. `issue_receipt()` creates a linear receipt, the implicit move
transfers its obligation, and `close_receipt()` discharges it.

```cpp
#include <ycxx/ownership.hpp>
#include <ycxx/ownership/runtime.hpp>

namespace own = ycxx::ownership;

struct Receipt {
  own::linear_state state;
};

Receipt issue_receipt() noexcept { return Receipt{}; }
void close_receipt(Receipt&& receipt) noexcept { receipt.state.consume(); }

using Rules = own::use_policy<own::linear<Receipt>, own::move_transfers<Receipt>,
    own::creates<decltype(&issue_receipt), &issue_receipt>,
    own::consumes<decltype(&close_receipt), &close_receipt>>;

int main() {
  auto issued = issue_receipt();
  Receipt current(static_cast<Receipt&&>(issued));
  close_receipt(static_cast<Receipt&&>(current));
}
```

```sh
c++ -std=c++20 -I/path/to/include receipt.cpp -o receipt
./receipt
ycxx cxx -fcheck-policies -fsyntax-only -I/path/to/include receipt.cpp
```

Remove `close_receipt()` from `main` to see the difference in enforcement. YCXX diagnoses the
live receipt at compile time. A GCC or Clang build traps at scope exit.

### Declare the receipt policy with reflection

Keep the includes, namespace alias, and `main` from `receipt.cpp`. Replace the resource and
operation declarations and the `Rules` alias with the declarations below. The attributes
register the same contracts in YCXX. GCC and Clang compile the guarded declarations and use
the same `linear_state` at runtime.

```cpp
struct
#ifdef __ycxx__
[[=own::resource_mode::linear, =own::move_constructor::transfers]]
#endif
Receipt {
  own::linear_state state;
};

#ifdef __ycxx__
[[=own::operation::creates]]
#endif
Receipt issue_receipt() noexcept { return Receipt{}; }

#ifdef __ycxx__
[[=own::operation::consumes]]
#endif
void close_receipt(Receipt&& receipt) noexcept { receipt.state.consume(); }
```

Use the same build commands for either form. In the typed form, the YCXX compiler discovers
the namespace-scope `Rules` alias when `-fcheck-policies` is enabled. Application code never
needs to refer to the alias.

### Verify shared observations

An observation marked `verified_observes<decltype(&inspect), &inspect>` asks YCXX to
verify the observer's body. An observer may return `void`, an arithmetic value, or an enum
value. A verified observer can return a field value or forward the original shared borrow
to another verified observer and return its result. It can also update independent scalar
storage. Bind a result to a local, such as `int value = inspect(owner);`, before using it in
a larger expression. For separate builds, include the contract in the caller and provider and pass
`-fcheck-policies` when compiling each object and at the final YCXX link. The checked link
supports explicit objects and direct calls to unique strong definitions. GCC and Clang accept
the metadata as ordinary C++20 types.

A local aggregate can hold independent owners in ordinary fields and fixed arrays. Initialize
each owner with its registered factory or transfer. Use the field path and an integer literal
for any array index when borrowing or consuming it. A nonthrowing conditional or comma initializer
can select the owner for a field or an array element in a checked native build.
[Compositional ownership](../../docs/design/policies-ownership.md#owning-fields)

## Local variants

Include `<ycxx/ownership/variant.hpp>` for a local choice between two owner types.
Construct `ownership::variant<Receipt, Permit>` with `ownership::first_t{}` and a receipt
or with `ownership::second_t{}` and a permit. Each alternative needs a nonthrowing
move and destruction when it is selected.

Use `holds_first()` or `holds_second()` to identify the active alternative. The
corresponding accessor borrows it by const reference. End the borrow before taking
the owner or leaving the variant's scope. `take_first()` or `take_second()` moves
the owner out and leaves the variant empty. Access to an inactive alternative traps.
Scope exit destroys the active alternative, so a guarded linear owner still has to
be discharged. The variant itself stays in its original scope.

GCC and Clang run the portable C++20 variant. YCXX checks ownership of the active alternative
in syntax-only builds and runs checked executables natively on arm64 macOS. Native x86-64 Linux
builds depend on support for the by-value selection tags. [ABI support #641](https://github.com/yrashk/ycxx/issues/641)

To check the active alternative with YCXX, compose the two resource policies, their
move-transfer contracts, and the variant's storage contract.

```cpp
namespace own = ycxx::ownership;
using Choice = own::variant<Receipt, Permit>;
using CheckChoice = own::use_policy<
    ReceiptPolicy, PermitPolicy,
    own::move_transfers<Receipt>, own::move_transfers<Permit>,
    own::tracks_variant<Choice>>;
```

`ReceiptPolicy` and `PermitPolicy` register the owner types and any factory, observation,
and terminal operations they use. `tracks_variant` opts into the package's tagged
storage protocol. Each move-transfer contract authorizes the corresponding resource
move, including moves into and out of that storage. With `-fcheck-policies`, a
`holds_first()` or `holds_second()` condition narrows the possible alternatives for
its branches. Pass an accessor result to a registered observer or bind it to a scoped
shared reference. Use the result of a take operation to initialize an owner or pass it
to a registered consumer. A live shared borrow prevents extraction, and a live linear
alternative must be discharged or transferred on every path out of scope. A scoped capability
token can be an alternative. Its registered observers can keep effect bounds and usage grades
when called from that branch.

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
moved-from token, or a released token. GCC, Clang, and YCXX enforce the same rights constraints.
On Linux x86-64, a checked YCXX object and a GCC- or Clang-built object can pass a token by
value in either direction. Use an ordinary link for this mixed build.
[Indirect results #661](https://github.com/yrashk/ycxx/issues/661) · [Type friendship #689](https://github.com/yrashk/ycxx/issues/689)

Register the token itself or an application wrapper as affine to check capability use and
ownership in one YCXX build. For direct registration, the issuer's static `noexcept` factory
returns a fresh token directly, as in `return Token(scope);`. Declare the namespace functions
that issue, observe, and release the token. The observation calls `require` and can return a
copied value. The registered release consumes the token, and YCXX rejects a later observation.
These declarations and runtime guards use the same C++ types in standard C++20 builds.

## Transitive effect bounds

Include `<ycxx/effects.hpp>` and give each effect an unqualified class or enum tag. An effect
set lists everything a function may require. Use `bound` to have YCXX verify a function's
body and `trusted` to describe the effects of a reviewed boundary adapter. A trusted
adapter can call `require` on a capability before accessing its resource.

```cpp
#include <ycxx/effects.hpp>

namespace fx = ycxx::effects;
struct ReadSettings;
using Reads = fx::set<ReadSettings>;

int read_settings() noexcept;  // supply the mediated adapter as a source input
int retry_read(bool retry) noexcept {
  if (retry) read_settings();
  return read_settings();
}

using Rules = fx::use_policy<
    fx::trusted<decltype(&read_settings), &read_settings, Reads>,
    fx::bound<decltype(&retry_read), &retry_read, Reads>>;
```

With `-fcheck-policies`, each call in a verified body requires a declared callee bound
contained in the caller's bound. For example, changing `retry_read` to `fx::set<>` rejects
the call to `read_settings`. GCC and Clang accept the same C++20 metadata, and the
capability guard in the adapter still runs.

Use `[[=fx::contract<Reads>{}]]` on a C++26 function declaration or name the contract
type with an alias and write `[[=ReadsContract{}]]`. Use `fx::trusted_contract<Reads>`
the same way for a trusted adapter. Tags are canonical types. Their order and repetition
do not change the meaning. Ownership and effect entries can share one `use_policy`
fragment across source inputs.

Start with `ycxx cxx -fcheck-policies -fsyntax-only -I/path/to/include adapter.cpp caller.cpp`.
For separate builds, compile the caller and provider with `-fcheck-policies -c`, then pass
the explicit objects to `ycxx cxx -fcheck-policies` at the final link. The provider's
verified bound must match the declared bound used by the caller. A trusted adapter must
use the same declared bound at every checked call site. The link also accepts sources
alongside objects.

A shared observer can carry both `ownership::verified_observes` and `effects::bound`.
When it forwards the borrow to another verified observer, the checked call carries both
the ownership and effect requirements to the final link. The selected provider must
satisfy both contracts.

The initial subset supports direct calls to namespace functions with scalar or reference parameters and a
scalar, reference, or void result. It checks default argument calls and diagnoses indirect
calls, volatile accesses, and unmodeled operations such as class initialization and cleanup.
`fx::set<>` permits zero declared effects. Ordinary scalar computation and memory accesses
are within this subset. [YCXX Policies tracking issue #660](https://github.com/yrashk/ycxx/issues/660)

## Runtime usage budgets

Include `<ycxx/effects/runtime.hpp>` to give a scope a finite budget for one effect.
Pass the same budget by reference to every mediated adapter that should share its limit.
The adapter calls `spend()` for each use. Exhausting the budget traps on the next attempted
spend. Declare the limit in the budget's type, for example
`ycxx::effects::budget<ReadSettings, 3>`. The counter cannot be copied or moved.
The adapter still needs a capability for the resource it accesses.

## Static usage grades

Use `fx::set<fx::at_most<ReadSettings, 3>>` as the bound to give a function a finite
maximum of three reads per call. The same set works with `fx::bound`, `fx::trusted`,
and their declaration annotations. In a checked body, sequential calls add their
declared costs. A conditional counts its condition and the more expensive branch.
A loop with a positive cost on a path to another iteration exceeds a finite grade.
If only a path that breaks or returns spends an effect, that cost counts once. A
`do` loop with a literal `false` condition executes at most once. A bare effect tag
declares an unbounded count and cannot satisfy a finite caller grade for that tag.
The checked linker compares graded contracts across explicit objects.

## Scoped handlers

Include `<ycxx/effects/handler.hpp>` to bind a callable to an effect. For example, construct
`fx::scoped_handler<ReadSettings, decltype(read_from_memory)> handler(read_from_memory)`
and pass it by reference to the adapter. The adapter invokes `handler.perform(args...)`.
The caller keeps the callable alive for the scope. A nested call can receive a different
handler for the same effect, while the enclosing handler keeps its original implementation.

With `-fcheck-policies`, a checked function can bind a local handler to a named function
and call `perform` directly. YCXX verifies the wrapper and follows the selected function's
outward effects and usage grades. The local handler can only appear as the receiver of
its own `perform` calls. A nested binding can select another function with the same
signature and a different outward bound.

Save this C++20 example as `handler.cpp`. The memory implementation has an empty
outward effect bound, so `run` can keep the same bound.

```cpp
#include <ycxx/effects.hpp>
#include <ycxx/effects/handler.hpp>

namespace fx = ycxx::effects;
struct ReadSettings;
using Pure = fx::set<>;

int read_from_memory(int offset) noexcept { return 40 + offset; }

int run(int offset) {
  fx::scoped_handler<ReadSettings, decltype(read_from_memory)> handler(read_from_memory);
  return handler.perform(offset);
}

using Rules = fx::use_policy<
    fx::bound<decltype(&read_from_memory), &read_from_memory, Pure>,
    fx::bound<decltype(&run), &run, Pure>>;

int main() { return run(2) != 42; }
```

Run it with a C++20 compiler or check the declared effects with YCXX.

```sh
c++ -std=c++20 -I/path/to/include handler.cpp -o handler
./handler
ycxx cxx -fcheck-policies -fsyntax-only -I/path/to/include handler.cpp
```

A trusted mediation adapter can declare which effects escape its handler. YCXX checks
its callers against that declaration. The runtime scope and the optional budget use the
same types with GCC, Clang, and YCXX.

### Check a reflected handler's usage grade

Save this example as `reflected_handler.cpp`. The handler spends one `Audit` from the
caller's budget before returning a setting. The reflection annotations declare that
outward cost on the trusted adapter, the handler implementation, and the caller.

```cpp
#include <ycxx/effects.hpp>
#include <ycxx/effects/handler.hpp>
#include <ycxx/effects/runtime.hpp>

namespace fx = ycxx::effects;
struct ReadSettings;
struct Audit;
using AuditBudget = fx::budget<Audit, 1>;
using OneAudit = fx::set<fx::at_most<Audit, 1>>;

#ifdef __ycxx__
[[=fx::trusted_contract<OneAudit>{}]]
#endif
int record_read(AuditBudget& audit, int offset) noexcept {
  audit.spend();
  return offset;
}

#ifdef __ycxx__
[[=fx::contract<OneAudit>{}]]
#endif
int read_from_memory(AuditBudget& audit, int offset) noexcept {
  return 40 + record_read(audit, offset);
}

#ifdef __ycxx__
[[=fx::contract<OneAudit>{}]]
#endif
int run(AuditBudget& audit, int offset) {
  fx::scoped_handler<ReadSettings, decltype(read_from_memory)> handler(read_from_memory);
  return handler.perform(audit, offset);
}

int main() {
  AuditBudget audit;
  return run(audit, 2) != 42;
}
```

```sh
c++ -std=c++20 -I/path/to/include reflected_handler.cpp -o reflected_handler
./reflected_handler
ycxx cxx -fcheck-policies -fsyntax-only -I/path/to/include reflected_handler.cpp
```

Add `handler.perform(audit, offset);` before the return in `run`. YCXX reports that
the second call exceeds the `Audit` grade. In a GCC or Clang build, both calls share
the same budget and the second call traps.

To configure the whole handler, replace the `AuditBudget` alias with these declarations.
The class configuration supplies the same limit to the runtime budget and the checked bound.

```cpp
template<class Effect, class Config>
  requires (Config::slots > 0) && (Config::slots <= 4)
struct HandlerPolicy {
  using Budget = fx::budget<Effect, Config::slots>;
  using Bound = fx::set<fx::at_most<Effect, Config::slots>>;
};

struct SiteConfig { static constexpr unsigned slots = 2; };
using AuditPolicy = HandlerPolicy<Audit, SiteConfig>;
using AuditBudget = AuditPolicy::Budget;
using AuditBound = AuditPolicy::Bound;
```

Use `[[=fx::contract<AuditBound>{}]]` on `run` and keep the `OneAudit` annotations
on the individual operations. With `slots = 2`, two calls to `perform` succeed. A
third call exceeds the declared grade in YCXX and traps in GCC or Clang builds.
