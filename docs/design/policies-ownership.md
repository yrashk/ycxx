# Ownership checking with YCXX Policies

See [the portable ownership library proposal](ycxx-policies.md) for user-defined policies,
runtime enforcement, capabilities, and effects.

`-fcheck-policies` checks the example protocol in `examples/affine/permit.hpp`. The package uses
ordinary C++. Its header activates a policy defined with `<ycxx/ownership.hpp>` from
[YCXX Policies](../../packages/ycxx-policies/README.md). Projects
can combine their own fragments in a namespace-scope alias of `use_policy<...>`. The compiler
selects operations by resolved declaration and canonical signature.
`issue()` creates a permit, `inspect(const Permit&)` observes one, and `redeem(Permit&&)` consumes
one. Forming an xvalue leaves the permit live. The package supplies trusted operation bodies for
this experiment.

Astra proposed demanding the check from the build root after `TuFront` and before the syntax-only
return. The check reads the typed function definitions, including concrete template instances. A
query on the typed body, local table, and signature returns an item-relative finding. The enclosing
unit query renders it at the source revision's position. The opt-in belongs to the build request,
so checked and ordinary requests can reuse the same frontend results.

To combine annotations or active policy fragments from several translation units, pass their
sources together, such as `ycxx cxx -fcheck-policies -fsyntax-only contracts.cpp caller.cpp`.
All source inputs in that invocation see the combined contracts for externally linked entities.

The same flag checks `examples/affine/transaction.hpp`. `begin()` creates a linear `Transaction`.
`transfer(Transaction&&)` moves its obligation into a new named local. `commit(Transaction&&)`
and `rollback(Transaction&&)` discharge it. An affine permit may go out of scope live. A linear
transaction must have transferred or discharged its obligation when its owner goes out of scope.

The typed body is projected into a control-flow graph of ownership operations and scope exits.
States for each local distinguish absence, a live obligation, and a spent value. Joins preserve
every incoming possibility. Analysis repeats loop backedges until these sets stop growing.
Declarations issue a fresh obligation each time execution reaches them. `return`, `break`,
`continue`, and fallthrough carry cleanup for the scopes they leave. A call that may unwind while a
transaction is live is diagnosed, including when the call takes no transaction argument. Astra
recommended this conservative rule for the first milestone before modeling exception handlers.

The prototype covers automatic locals, by-value parameters, `if`, `while`, `do`, `for`, and
conditional, short-circuit, and comma expressions. Boolean literals select the reachable branch.
Operations take named owners, with parentheses and qualification conversions such as
`static_cast<Permit&&>(p)` allowed. A separate `move_transfers<Resource>` entry authorizes
direct initialization from a named owner through the resource's ordinary nonthrowing move
constructor. The equivalent class annotation is `[[=ycxx::ownership::move_constructor::transfers]]`.
These declarations trust the constructor to move the live obligation and invalidate the
source. YCXX checks that the named source is live and unborrowed before the call.
A local `const Resource&` can borrow a live named owner
from its declaration to the end of its scope. It can be passed to a registered observation
operation. A consume or transfer of the owner is an error while any of its borrows can still be
live, including when the reference's last use was earlier. Ending the borrow leaves the
ownership obligation with the owner. References bound to temporaries, mutable aliases,
reborrowing, captures, and exposing an address are outside this checked protocol. Observation
contracts declared with `observes` trust the implementation to keep the reference within the
call. Use `verified_observes<decltype(&inspect), &inspect>` in a policy or
`[[=ycxx::ownership::operation::verified_observes]]` on the function to check its definition.
When composed with a trusted observation contract for the same function, verification is required.
An observer may return `void`, an arithmetic value, or an enum value. A verified observer can
copy an arithmetic field into its result or to an independent scalar variable. It can also
forward the original shared reference and return the next verified observer's result. The checker
rejects address escape, mutation, capture, and calls outside this verification chain.
Checked callers can bind a copied result to a local before using it in a larger expression.
A syntax-only check needs the unique supported observer definition in the same invocation. For
separate builds, include the observer contract in the caller and provider, compile both with
`-fcheck-policies -c`, and pass the flag again at the final YCXX link. The checked link supports
direct calls to unique strong definitions in explicit objects. A forwarding observer carries
its own guarantee and a requirement for the observer it calls. Exception handlers and jumps
outside the supported loop control also receive an error.

## Capability token ownership

A directly registered affine `ycxx::capabilities::token<Issuer, Rights>` can be constructed by
its issuer's static `noexcept` function with a single direct return from the scope constructor.
The compiler checks the scope expression for ordinary resource use. Callers acquire the token
through a namespace-scope adapter registered with `creates`, and observation and consumption use
their registered adapters. The token's `require` reads its scope by value and calls the runtime
state's `observe` on its own state field. Its `release` calls `consume` on that field. Other
uses of the token receiver, including taking a backing field's address, receive ownership checks.
[Direct token lifecycle #799](https://github.com/yrashk/ycxx/issues/799) ·
[Member template exception specifications #804](https://github.com/yrashk/ycxx/issues/804)

## Owning fields

A local aggregate can hold multiple registered owners alongside arithmetic or enum fields and
can nest other supported aggregates. Initialize each owning field with a registered factory or
transfer. YCXX tracks the full field path, such as `tree.left.work`, and checks each owner
independently through observation, borrowing, transfer, consumption, and scope exit. It checks
the initializers in field order, so a later initializer that can unwind is diagnosed if a linear
field is already live. Scalar fields can be read by value.

```cpp
struct Pair {
  ycxx_affine::Transaction first;
  ycxx_affine::Transaction second;
};

void finish_pair() {
  Pair pair{ycxx_affine::begin(), ycxx_affine::begin()};
  ycxx_affine::commit(static_cast<ycxx_affine::Transaction&&>(pair.first));
  ycxx_affine::rollback(static_cast<ycxx_affine::Transaction&&>(pair.second));
}
```

The field protocol covers non-overlapping value fields and trivial or implicit nonthrowing
destruction. Whole-record copying and transfer, unions, and bases that contain owners
require additional checking.

## Fixed arrays of owners

Fixed arrays can hold registered resources or aggregate elements containing owners. Arrays
and fields can nest in a local aggregate, so a path such as `queue.groups[0].tasks[1]`
identifies one obligation and its borrows. Use aggregate initialization and an in-bounds
integer literal for every array index in an owner path. Scope exit reports the full path
of any linear owner that remains live.

Consuming `tasks[1]` can proceed while `tasks[0]` has a shared borrow. The array and
aggregate protocol requires nonthrowing destruction. Pointer aliases and runtime indices
require additional checking.

In a checked initializer, `choose ? begin() : handoff(static_cast<Work&&>(previous))`
checks each alternative as its own ownership path. In `(inspect(previous), begin())`,
the left operand runs before the new owner is initialized. This works for direct
owners, owning fields, and fixed array elements in syntax-only checks. Native
initialization supports nonthrowing class results returned directly by function calls.
[Class-result lowering #801](https://github.com/yrashk/ycxx/pull/801)

## Tagged owner alternatives

`tracks_variant<Choice>` registers the package's `ownership::variant` specialization.
The alternatives use their own affine or linear policies and move-transfer contracts.
The checker records the selected alternative and refines that state with `holds_first()`
and `holds_second()`. A scoped reference may observe the active alternative. Extraction
ends that alternative's ownership in the variant and transfers it to the destination.
An active linear alternative must be discharged before scope exit or unwinding.

The variant protocol works in syntax-only checks and checked arm64 Darwin links.
Native x86-64 Linux lowering of the selection tags is tracked in
[#641](https://github.com/yrashk/ycxx/issues/641).

## Try it

From the repository root, save the following as `permit-demo.cpp`.

```cpp
#include "permit.hpp"

void run(bool early) {
  auto p = ycxx_affine::issue();
  ycxx_affine::inspect(p);
  if (early) ycxx_affine::redeem(static_cast<ycxx_affine::Permit&&>(p));
  ycxx_affine::redeem(static_cast<ycxx_affine::Permit&&>(p));
}
```

```sh
build/bin/ycxx cxx -I packages/ycxx-policies/include -I examples/affine \
  -fcheck-policies -fsyntax-only permit-demo.cpp
```

The final call reports a possible second consumption and the earlier call's location. An
`else` in front of the final call makes the two consumptions mutually exclusive.

This example observes through a shared borrow and consumes the permit after that borrow ends.

```cpp
#include "permit.hpp"

void inspect_then_redeem() {
  auto permit = ycxx_affine::issue();
  {
    const auto& view = permit;
    ycxx_affine::inspect(view);
  }
  ycxx_affine::redeem(static_cast<ycxx_affine::Permit&&>(permit));
}
```

The same invocation checks this linear example when saved as `permit-demo.cpp`.

```cpp
#include "transaction.hpp"
using namespace ycxx_affine;

void finish(bool success) {
  auto tx = begin();
  if (success) commit(static_cast<Transaction&&>(tx));
  else rollback(static_cast<Transaction&&>(tx));
}
```

Removing `rollback` leaves the transaction live on the false branch. The check reports the
scope exit. A transfer into another local moves the obligation to that local and invalidates `tx`.

`examples/affine/runtime_transaction.hpp` uses the portable runtime guard from
`<ycxx/ownership/runtime.hpp>`. Ordinary C++ compilers can compile the adapter. Its guard
reports unfinished linear ownership and repeated consumption on executed paths. With
`-fcheck-policies`, YCXX also checks callers against the adapter's activated policy. Borrow
checking uses the source reference and its scope at compile time. Ordinary C++ builds use the
same reference and object layout and retain the runtime guard on the adapter's operations.
