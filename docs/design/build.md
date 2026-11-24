# Builds through the resident engine

Epic #1251. Design issue #1252. This document proposes an initial contract and
an executable test of it.

## First build

Build an executable, one library and a generator using one host configuration.
The library and executable export named artifacts. An edge for a generated
header orders the generator before compilation. The edge for the library
object or archive orders linking independently of header production. A
configured target has a stable workspace name and configuration. Local names
can be ordinary strings. Generated output paths belong to the build rather
than the source checkout.

The first build runs the expected tools. The second launches zero tools.
Changing only one generator output schedules only its consumers. A newly
created header that shadows an old include invalidates the compile. A failed
tool cannot publish a partial result. After each edit, compare the runnable
output with a fresh build of that workspace.

## Build definitions and tracked values

The initial grammar is declarative (#1253). A target carries its rule name,
local name, located fields and named artifact references. Explicit globs expand
against tracked directory listings. Match order is stable, exclusions are
visible, and recursion uses an explicit pattern. Track membership of observed
directories, including previously absent roots. File-content misses alone
cannot account for files whose names a glob has never seen. The runner handles
artifact references to outputs even before their files exist. Locations and expansion
reasons are provenance, which can change without changing a resolved value.

A build root demands the target's exported artifacts. Internally, give
planning, tool results and output projections independent observation points.
In particular, a compile that only reads one generated header depends on that
header's bytes and relevant metadata. It can remain green when the generator
runs and a sibling output changes. Stable input keys and canonical results
permit the red/green engine to reuse its normal verification and cutoff.

## Side effects

The action boundary (#1254) accepts a tool identity, structured arguments,
environment, staged input snapshots and named output paths. The tool runs
in a private working directory. Verify its declared outputs and return them
as one successful result. The runner owns process lifecycle and diagnostics.
The caller owns cache admission and materialization. A crash, cancellation
or failed command produces no reusable successful result. The first runner
can return owned bytes, before deciding on durable artifact storage.

Keep subprocess execution outside ordinary reevaluation. A query returns
pending action demands to the build coordinator. The coordinator deduplicates
and executes them, admits complete receipts as tracked inputs at quiescence,
and requests the root again under the same outer trace. Waiting inside the
epoch for a receipt that requires the epoch to end would deadlock. Engine
verification rechecks queries using receipts without launching tools.
Publication uses a completed request's generation and can restore removed
output files from cached values.

## Compiler and trace

Native compilation should run as a descendant of the workspace root in the
same pipeline session database (#1255, #1256). The root can fetch
`pipeline::Build` through its query context. Use existing compiler
dependencies, including unsuccessful include lookups. The preprocessor's
include callback is synchronous, so the native adapter first fetches the
declared generated-output projections and prepares an immutable file view.
The callback keeps observing earlier disk candidates in search order. Cover
the compiler's separate main-file text and readability paths before
supporting generated main sources.

An external compiler path needs an explicit input envelope including its
include search space and toolchain identity before its results can be reused.
It is a user-authored action, subject to a different contract from the native
toolchain in DECISIONS §7. Account for the SDK where appropriate. Native
Darwin hosted linking currently resolves libSystem SDK metadata once per
process. Add finer external discovery once its observations suffice for
validation.

Astra independently checked the native root, generated-input and action
receipt boundaries for #1252.

Carry the existing request tracer. Keep `--trace-engine=interactive`,
`--trace-engine=stream`, and `--trace-engine=json` (JSON Lines on stderr).
A reused tool result should be reported as reuse. Native compiler descendants
retain their existing query events. A subprocess boundary can report the
invocation, diagnostics and observed input/output identities. The resolved
view should expose source and expansion provenance, support semantic diffs,
and share its data with the CLI and eventual HTML view (#1256).

## Extensions and broader project layouts

A declared command action covers basic foreign integrations. A planner can
request engine-observed files, directory listings and action results to emit
more actions. A package can declare and build a host rule plugin and import
its exports on demand (#1257). The bootstrap graph must be acyclic. Load a
plugin generation into a rule worker through a versioned interface, with a C++
SDK and structured values. The first plugin provider can use an explicit host
toolchain capable of producing a shared library. Building the plugin through
ycxx does not require the native compiler to produce that library initially. The implementation digest invalidates rule
evaluation. Equivalent emitted actions keep their existing identity. Only
tracked observations justify planner reuse. Custom semantic queries can
publish independently comparable facts when a consumer exists for them.

A shader inventory provides the extension witness (#1258). It defines a
runtime name for each source. The rule emits one shader compile per object and
a bundle action. The executable uses the result as a runtime asset. A shader
edit can change its SPIR-V without relinking the application. A structured
reflection query can keep downstream bindings green if its relevant fields
stay equal.

Modules, host and target variants, richer link propagation and a foreign build
adapter grow coverage after the first witness (#1259). Package acquisition and
version solving sit outside the build.
