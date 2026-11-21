# Portable C++ ownership, capabilities, and effects

Proposed design. Applications use one ordinary C++20 library and declare one set of contracts for
resource APIs. Any conforming C++ compiler enforces the constraints expressed by the public types
and templates. Library adapters check the remaining obligations on executed paths. With checking
enabled, YCXX verifies those contracts against the control flow and calls of the program before
emitting code. Checked and unchecked translation units use the same declarations, layouts, and
runtime behavior. The first implementation keeps the runtime checks enabled in both builds.

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
selected overload. The public header is `include/ycxx/ownership.hpp`.

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

The first contract vocabulary covers creation, observation, transfer, and terminal consumption.
Extend it with parameter and result positions, receiver effects, borrow lifetimes, and outcomes as
adapters need them. Registry validation checks signatures, resolves conflicting contracts, and
rejects unknown operations in checked contexts. Check adapter definitions against their contracts
where the checker can see their bodies and recognize the ownership primitives they use. A contract
for an opaque implementation is a trust decision. The metadata for an existing dependency can
live in an adapter header. Portable dynamic enforcement requires routing the operation through a
wrapper that tracks the resource. Reflection annotations can provide another spelling for the same
metadata. Class and operation declaration annotations are tracked by
[#662](https://github.com/yrashk/ycxx/issues/662). The complete reflection surface is tracked by
[#324](https://github.com/yrashk/ycxx/issues/324).

## Enforcement by ordinary C++, the runtime, and YCXX

| Contract | Ordinary C++ types | Portable runtime guards | YCXX static analysis |
| --- | --- | --- | --- |
| Affine ownership | Reject copying a move-only handle | Reject use after transfer or consumption | Reject a use when any incoming path has already transferred or consumed it |
| Strict linear ownership | Expose the legal terminal operations | Report a live obligation when the owner is destroyed | Verify discharge or transfer on every modeled exit |
| Capabilities | Require a capability of the right type at the API boundary | Validate instance rights, resource scope, revocation, and optional budgets | Verify capability passage and delegation through checked code and contracts |
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
bodies. See [the current prototype](affine-permit.md) and the [query engine design](query-engine.md).

## First conformance gate

Use the same resource definition, adapter operations, and caller source on GCC, Clang, and YCXX.
The decisive fixture creates a transaction, returns early based on a runtime argument, and commits
on the other path. GCC and Clang should accept the C++ source. The commit path should succeed and
the early path should reach the violation handler. YCXX should reject the checked caller and point
to the early return. Add fixtures for transfer followed by source reuse, an unclaimed result,
exception exit, a direct copy rejected by ordinary C++, and a capability check through an indirect
callee. Keep runtime observations and static verdicts in the same conformance suite.

A scratch probe confirmed that the typed registration alias and runtime-adapter source pass YCXX
syntax analysis. GCC and Clang ran the valid path and trapped the early return. The current YCXX
prototype still recognizes its two example protocols. The probe's native Linux build hit the
nontrivial class result ABI gap tracked under [#440](https://github.com/yrashk/ycxx/issues/440).
