# Affine and linear ownership prototype

See [the portable ownership library proposal](ownership-library.md) for user-defined policies,
runtime enforcement, capabilities, and effects.

`-fcheck-affine` checks the example protocol in `examples/affine/permit.hpp`. The package uses
ordinary C++. The compiler selects the protocol by resolved declaration and canonical signature.
`issue()` creates a permit, `inspect(const Permit&)` observes one, and `redeem(Permit&&)` consumes
one. Forming an xvalue leaves the permit live. The package supplies trusted operation bodies for
this experiment.

Astra proposed demanding the check from the build root after `TuFront` and before the syntax-only
return. The check reads the typed function definitions, including concrete template instances. A
query on the typed body, local table, and signature returns an item-relative finding. The enclosing
unit query renders it at the source revision's position. The opt-in belongs to the build request,
so checked and ordinary requests can reuse the same frontend results.

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
`static_cast<Permit&&>(p)` allowed. Alias declarations, captures, indirect calls involving an owner,
exception handlers, and jumps outside the supported loop control receive an error. The examples
use trusted implementations of the declared operations.

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
build/bin/ycxx cxx -I examples/affine -fcheck-affine -fsyntax-only permit-demo.cpp
```

The final call reports a possible second consumption and the earlier call's location. An
`else` in front of the final call makes the two consumptions mutually exclusive.

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
