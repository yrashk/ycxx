# YCXX Policies design

YCXX Policies is a standalone C++20 package for ownership, capabilities, and effects. Applications
declare one set of contracts for resource APIs. Any conforming C++20 compiler enforces the constraints
expressed by the public types and templates. Library adapters use runtime guards on executed paths.
With `-fcheck-policies`, YCXX also verifies contracts against the control flow and calls before
emitting code. Runtime guards default to enabled. Applications can compile them out with
`YCXX_POLICIES_RUNTIME_CHECKS=0` while keeping the same types and layouts and any YCXX static
checks they requested. Use the same runtime setting throughout an executable.

## Ownership, authority, and effects

**Ownership** governs the lifetime of an obligation. Creating an owner starts one obligation.
Moving it transfers that obligation to a new place. A terminal operation discharges it. A borrow
permits access for a bounded lifetime without transferring the obligation. The number of transfers
can be arbitrary. An affine owner permits implicit drop. A strict linear owner requires a transfer
or terminal operation on every path that leaves its owning lifetime, including exception exits.

**A capability** grants a specific authority. A trusted issuer creates an opaque value with a grant,
such as writing within a particular directory. APIs take that value explicitly. The capability's
rights and resource scope describe what can be done. Its ownership describes whether the grant can
be shared, dropped, or used once. A shareable reader, an affine socket handle, and a linear
transaction are distinct useful policies.

**An effect** describes an operation a function may perform, including through its callees. A
function can borrow a write capability, declare a filesystem write effect, and return a linear
transaction. The write effect describes the operation, the capability authorizes it, and the
linear result creates a completion obligation.

Austral informs the use of explicit authority and trusted issuers. Ante informs effect contracts
and scoped handlers. Granule informs a later extension to graded usage and budgets. Each idea has
its own implementation milestone. [Austral specification](https://austral-lang.org/spec/spec.html#capability-based-security) · [Ante language tour](https://antelang.org/docs/language/#effects) · [Granule language](https://granule-project.github.io/granule.html)

## Public C++ API

Use resource handles with private construction, explicit factories, deleted copying where required,
and a move operation that clears the original owner's state. Put the portable runtime guard inside
the adapter that owns the real payload. Scope the first version to explicit borrows that cannot
escape a call. Use type constraints for rights and effect requirements expressed through typed
interfaces. Keep operations on existing domain types, such as `commit`, `rollback`, and `write`.

Register contracts by canonical type and selected declaration. Libraries publish reusable
`policy<...>` types. An application activates one or more fragments with a namespace-scope
alias of `use_policy<...>` in a translation unit. A typed function pointer identifies the
selected overload. The public header is `<ycxx/ownership.hpp>` in the standalone
[YCXX Policies package](../../packages/ycxx-policies/README.md).

```cpp
using transaction_policy = own::policy<
    own::linear<db::transaction>,
    own::creates<decltype(&db::begin), &db::begin>,
    own::consumes<decltype(&db::commit), &db::commit>,
    own::consumes<decltype(&db::rollback), &db::rollback>>;

using check_transactions = own::use_policy<transaction_policy>;
```

Fragments may contain other fragments. Composition is order-independent for compatible entries.
Equivalent declarations deduplicate. Conflicting kinds or operation contracts report the
declaration and its previous origin. The registry normalizes an active fragment once and uses
ordered lookup by canonical type and entity. A fragment can be shared across headers or left
inactive. Registry and body results are engine queries. A source edit that leaves the normalized
registry equal can reuse checks for unchanged bodies.

A single checked build combines the active policies of its source inputs before checking the
bodies in those inputs. For example, one source can register an externally linked resource
declared in a common header, a second can register its operations, and a third can call them.
The compiler resolves each entry in the scope where it was declared and validates operation
signatures after collecting the resource declarations. Internal-linkage operations apply in
their own translation unit. Diagnostic origins identify the source input as well as the
declaration within it, so conflicts can point to both inputs. Separate compiler invocations
have separate registries until verified object summaries are available.

The first contract vocabulary covers creation, observation, transfer, and terminal consumption.
Extend it with parameter and result positions, receiver effects, borrow lifetimes, and outcomes as
adapters need them. Registry validation checks signatures, resolves conflicting contracts, and
rejects unknown operations in checked contexts. Check adapter definitions against their contracts
where the checker can see their bodies and recognize the ownership primitives they use. A contract
for an opaque implementation is a trust decision. The metadata for an existing dependency can
live in an adapter header. Portable dynamic enforcement requires routing the operation through a
wrapper that tracks the resource. C++26 declaration annotations can provide the same metadata.

```cpp
namespace own = ycxx::ownership;

struct [[=own::resource_mode::linear]] Receipt { int value; };
[[=own::operation::creates]] Receipt open() noexcept;
[[=own::operation::consumes]] void close(Receipt&&) noexcept;

struct TransactionSelection { using Active = own::use_policy<transaction_policy>; };
struct [[=TransactionSelection::Active{}]] CheckTransactions {};
```

Resource annotations go on classes and operation annotations go on functions. An annotation
holding `use_policy<...>` activates fragments. Both spellings enter the same registry, including
duplicate and conflict checks. Class and operation declaration annotations are covered by
[#662](https://github.com/yrashk/ycxx/issues/662). The complete reflection surface is tracked by
[#324](https://github.com/yrashk/ycxx/issues/324).

## Verified contracts for separately compiled code

YCXX checks `verified_observes` definitions for leaf observation bodies that preserve the
resource and keep the reference within the call. A syntax-only check needs the provider in the
same invocation. A checked native build can compile a caller against its declared contract and
carry a requirement to the final checked link. That link matches the requirement against the
definition selected for the call. A verified observer can forward its original shared reference
to another verified observer and carries the corresponding requirement through the final link.

Astra proposed recording caller requirements and verified provider guarantees as versioned local
symbols attached to the corresponding function definitions. Both YCXX object formats retain
these symbols. The external ABI symbol, the object target, and a stable fingerprint of the
canonical signature and ownership policy identify a contract between compiler sessions.
Compile each explicit object with `-fcheck-policies` and use that flag at the final YCXX link.
Each ordinary input must have a coverage record that counts the associated requirements and guarantees.
Missing, incomplete, malformed, and incompatible records fail verification. The checked link
trusts unmodified compiler outputs and the usual C++ declaration and definition consistency.

The checked link supports explicit objects and source inputs. Each required direct reference
must have one strong definition, with matching evidence at that definition's text address.
The default Darwin link can also select members from the driver's libycxx runtime archives.
Their definitions participate in resolution and supply no policy promises.
Explicit archives, weak providers, and dynamic providers still need verification against the
linker's coalescing, member selection, and import rules. An ordinary link provides the portable
runtime enforcement described below. [#946](https://github.com/yrashk/ycxx/issues/946)

## Enforcement by ordinary C++, the runtime, and YCXX

| Contract | Ordinary C++ types | Portable runtime guards | YCXX static analysis |
| --- | --- | --- | --- |
| Affine ownership | Reject copying a move-only handle | Reject use after transfer or consumption | Reject a use when any incoming path has already transferred or consumed it |
| Strict linear ownership | Expose the legal terminal operations | Report a live obligation when the owner is destroyed | Verify discharge or transfer on every modeled exit |
| Capabilities | Require a token from the declared issuer with the required rights | Validate the token's scope and live state at a mediated operation | Verify the token's ownership through checked code and contracts |
| Effects | Constrain declared effect sets for typed callables | Deny ungranted effects at mediated operations on the executed path | Verify declared effect bounds against the body and transitive callees |

A portable owner stores a live state. A move checks and clears the source and arms the destination.
A checked operation tests the state before using the payload. A strict linear destructor calls a
nonthrowing violation handler if the obligation is live. The default handler terminates execution.
An auto-rollback transaction is a separate declared protocol whose destructor actually rolls back
and thereby discharges the obligation. Destructors detect violations where C++ invokes them, which
includes normal and exception scope exit. Abrupt termination and lost raw allocations can bypass
object destruction.

The first operations that transfer or discharge an obligation use `noexcept`. Fallible operations
later specify whether the owner is retained, returned, or discharged on each normal and exceptional
outcome. For example, a failed commit that requires recovery returns ownership to its caller. A
runtime borrow that can outlive a call needs a checked view with shared lifetime or generation
state. Releasing an unrestricted reference from that view gives up tracking of the reference.
Cross-thread borrows need synchronization or lifetime pinning as well.

## Authority and effect boundaries

Start by passing capabilities explicitly to effectful adapters. An authority issued for one scope
can be delegated only with the permitted rights and scope. Type constraints can verify a rights
subset. An adapter and its underlying system enforce instance-specific restrictions. Runtime
handlers can supply scoped effect implementations and enforce limits on those calls.

The portable package provides `token<Issuer, Granted>`, with the issuer as the only constructor
from a scope identifier. The `grants<Granted, Required>` constraint checks the rights subset.
The `require<Required>(scope)` operation also checks the numeric scope in the issuer's domain and
the affine live state. Each issuer controls authorization and assigns stable identifiers to its
mediated resources. Moving or releasing a token invalidates that token. [Package guide](../../packages/ycxx-policies/README.md#scoped-capabilities)

In YCXX, give functions declared upper bounds on their possible effects. Verify the body against
its bound, compose callee summaries, and carry callback effects through generic combinators. An
indirect call needs a verified bound for its targets. An external function's effects come from a
verified summary or a trusted adapter. An unknown operation has an unknown bound and fails a
checked context that promises a narrower one. A runtime denial catches an attempted operation.
A static bound also covers paths that were never executed.

A capability guarantee covers an explicitly checked build boundary whose protected operations all
pass through mediated APIs. Raw OS calls, opaque FFI, assembly, and unchecked memory operations are
trusted or rejected at that boundary. Running hostile native dependencies requires an enforcement
boundary outside the library, such as OS isolation. For handlers, begin with explicit scoped
contexts, then single-shot resumptions. Multi-shot resumption needs a restriction on copying live
ownership in the captured continuation. Allow only copyable captures in its first version.

### First transitive effect bounds

Astra proposed using canonical tag types as effect names and a deduplicated set of tags as a
function's upper bound. A filesystem read tag can be distinct from a network read tag. The
issuer's rights mask still governs which capability can authorize the operation. Both typed
portable declarations and reflection annotations should normalize to the same bound. Trusted
adapter declarations specify the effects at the mediation boundary.

The first implementation uses `effects::set<Tag...>` with `bound` and `trusted` policy
entries. The corresponding `contract` and `trusted_contract` annotations share the same
canonical representation. Equivalent declarations deduplicate. Differing bounds or trust
declarations conflict. A cached query records a body's call sites. Its verifier depends
on the normalized bound for the function and each callee. Every verified body is checked
against its own declaration, which also covers recursion without effect inference. The
typed tree includes calls in default arguments and omits discarded `if constexpr` arms.
Unknown calls and unmodeled implicit operations fail finite-bound checks. The first slice
requires unique strong external definitions or local definitions among the checked inputs.

For checked objects, separate effect records carry guarantees, requirements, and trusted calls.
They reuse the observation records' coverage count. The bound digest uses stable type
fingerprints in sorted order, so input order and process-local handles do not change it.
The checked link matches each requirement to the unique strong explicit provider with the
same digest. A forwarding object carries both its guarantee and its callees' requirements.
Trusted calls carry their digest for agreement across callers. A trusted call conflicts with
a verified bound for the same function. Archive extraction and dynamic providers remain
tracked in #660. [YCXX Policies #660](https://github.com/yrashk/ycxx/issues/660)

### First usage grades

`effects::at_most<Tag, N>` places a positive finite upper bound on a tag in an effect set.
A bare tag keeps its original unbounded meaning. An absent tag allows zero uses. Duplicate
entries agree when their canonical tag and count agree. An explicit finite bound and a bare
tag for the same effect conflict.

On standard C++20 compilers, a mediated operation can take an explicit
`effects::budget<Tag, N>&` and spend a use when it executes. Repeated calls share the same
budget by reference. The counter belongs to one scope and cannot be copied or moved. The
budget counts uses of an effect. The capability still supplies authority over the resource.

YCXX uses declared callee costs. Sequential operations add their upper bounds, and
alternative branches take the maximum after the condition. A `for` with an `int` or `unsigned`
counter can count up to a strict upper limit or down to a strict lower limit. YCXX recognizes
literal and constexpr static data member endpoints of the counter's type and requires the body to
use the counter only by value. YCXX multiplies repeating costs by the maximum iteration count,
including paths that break or return after earlier iterations. For other loops, a path that
reaches another iteration with positive cost is unbounded. Costs on a path that breaks or
returns are counted once when every path to another iteration has zero cost. The initializer in a
`for` runs once, and the increment runs only after ordinary completion of the body or `continue`.
An explicit `while (false)` condition ends a `do` after one iteration. An unbounded callee cannot satisfy a
finite grade. Separate object builds include grades in the effect contract digest. A change to
a declared grade invalidates summaries that depend on it.

### Explicit handler scopes

A portable `effects::scoped_handler<Tag, Implementation>` borrows a callable implementation
for one effect. The surrounding scope keeps the callable alive. Each adapter receives the
handler explicitly and invokes its implementation through `perform`. A nested scope can
pass a different handler for the same tag. Existing scopes retain their implementations.
A stateful callable can share a budget and hold the authority required by its operation.

A local handler can bind a direct namespace function. YCXX verifies that the selected
constructor only stores the reference and that `perform` only forwards its arguments to
that reference. The checked body cannot alias or export the local handler. Each call uses
the bound and grade of the selected function, and separate object builds record the same
requirements as direct calls to that function.

A trusted mediation adapter can still declare the outward bound for other callable
implementations. Captured resumptions require further ownership and effect checks.

## YCXX implementation milestones

1. **User registration and portable adapters.** Use typed policy aliases to replace the current
   permit and transaction name mappings. Build a registry as a query value and diagnose conflicting
   declarations. Ship an affine permit, a strict transaction, and a project-local resource as
   conformance cases. Include a separately compiled runtime adapter. Exercise the real destruction
   and move behavior with GCC and Clang. Verify that the same source can pass checked YCXX native
   builds once its calling convention is supported. [Native indirect results #661](https://github.com/yrashk/ycxx/issues/661)
2. **Compositional ownership.** Extend the existing typed-body graph and worklist to owned fields,
   arrays, active variants, captures, standard container adapters, move construction and assignment,
   and lifetime and cleanup on unwind. Verify contracts for parameters, receivers, and results.
   Consume verified summaries across translation units. Check borrows and escapes before supporting
   general references. Model coroutine suspension and destruction before allowing an owner to remain
   live across suspension.
3. **Capability and effect checking.** Add typed authority for two mediated effects, such as file
   read and file write. Verify effects across calls, templates, callbacks, and separately compiled
   code. Require explicit trust at the program boundary and reject ambient effects in checked code.
4. **Grades and handlers.** Add explicit budgets such as at most three authorized sends. Portable
   adapters count executed events. For static upper bounds, compose sequential costs by addition
   and alternative paths by maximum. Loops need a provable bound or an explicit invariant. Add
   effect polymorphism and structured handlers with linear ownership preserved at each resumption.

The checker belongs before code generation, including in `-fsyntax-only`. Query the registry,
instantiated typed bodies and their control flow, and verified per-function summaries. Use canonical
identities and include contract dependencies in the query keys. Editing a contract must invalidate
dependent proofs. An unrelated edit should reuse the ownership and effect results of unchanged
bodies. See [the ownership checker](policies-ownership.md) and the [query engine design](query-engine.md).

## Conformance gates

`tools/test --fast` builds YCXX and runs the policy fixtures through CTest. Run the standalone
package fixture separately with GCC and Clang. It configures a consumer using the exported target
in both installation and vendor layouts. The issue records the exact branch revisions and native
hosts that passed. [Project record #660](https://github.com/yrashk/ycxx/issues/660)

| Gate | Required coverage | Fixtures |
| --- | --- | --- |
| Portable package | C++20 installation, relocation, vendoring, ordinary type constraints, guarded and unguarded runtime behavior, and identical sizes and alignments across guard settings. | [Package](../../tests/unit/pipeline/policies_package.py) |
| Ownership and borrows | Affine drops, linear discharge at scope exit, authorized transfers, owned fields and arrays, lexical shared borrows, rejected escapes, and tracked alternatives. Exercise the same resource definitions and adapters in runtime and static tests. | [Ownership](../../tests/unit/pipeline/policies_ownership.py) · [Variants](../../tests/unit/pipeline/policies_variants.py) · [Contracts](../../tests/unit/pipeline/policies_contracts.py) |
| Separate translation units | Publish, require, and match verified contracts across objects. Reject missing or changed guarantees. Exercise mixed-compiler calls in both guard settings and cleanup with the package ABI. | [Contracts](../../tests/unit/pipeline/policies_contracts.py) · [Effects](../../tests/unit/pipeline/policies_effects.py) · [Grades](../../tests/unit/pipeline/policies_grades.py) |
| Capabilities, effects, and grades | Check mediated authority, transitive effect bounds, scoped handlers for supported callables, and finite usage counts. Compare runtime traps and static rejections, including static enforcement with runtime guards disabled. | [Effects](../../tests/unit/pipeline/policies_effects.py) · [Grades](../../tests/unit/pipeline/policies_grades.py) |
| Composition and incremental reuse | Combine typed and reflected declarations across headers and translation units. Verify equivalent normalization, both conflict origins, and reuse for 1,280 resources and 144 effect tags. Change a policy header or provider while preserving size and timestamp. Toggle runtime guards in one daemon. Compare the resulting diagnostics and binaries with cold builds. | [Contracts](../../tests/unit/pipeline/policies_contracts.py) · [Effects](../../tests/unit/pipeline/policies_effects.py) · [Grades](../../tests/unit/pipeline/policies_grades.py) · [Daemon](../../tests/daemon/process.py) |

The native owner-variant gate runs both valid alternatives, a capability scope violation, and a
linear leak through a variant. It runs on arm64 macOS. The Linux fixture currently asserts the
compiler's empty-selection-tag ABI diagnostic. Once the lowering lands, it must run those same
native cases on x86-64 Linux. [Empty selection tags #641](https://github.com/yrashk/ycxx/issues/641)

The mixed-compiler unwind gate uses an automatic aggregate with a nonthrowing destructor around
a direct external call. A GCC or Clang caller creates the guard, and the YCXX helper consumes it
on normal return and exception propagation. [Indirect results #661](https://github.com/yrashk/ycxx/issues/661) · [Exception cleanup #663](https://github.com/yrashk/ycxx/issues/663)
