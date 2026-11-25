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

### Using the first slice

Place `build.ycxx` in the working directory.

```text
cpp.library "core" {
    sources = ["core.cc"]
    public_include_dirs = ["include"]
}

cpp.executable "app" {
    sources = glob("src/*.cc", exclude = ["src/*_test.cc"])
    deps = ["core"]
}
```

`ycxx build app` prints the path to `.ycxx/out/app` on success. Use
`--trace-engine=stream`, `--trace-engine=interactive`, or `--trace-engine=json`
to inspect the request. `ycxx build app --watch --trace-engine=interactive`
keeps successive builds in one trace. Ordinary source and include changes use
the daemon's file watcher. While waiting, the daemon also reobserves the
project plan so manifest changes, generator inputs, and new glob matches can
wake the next build. A failed build leaves the last executable available and
the watch continues. The library in this slice is an ordered group of
native objects included in the executable link. The generator example in
`tests/daemon/project_build.py` shows how to bind named staged paths with
`input("name")` and `output("name")`. Tools must use the inputs declared by
their rule. The first runner uses an explicit host executable and a private
working directory. The coordinator rechecks the executable fingerprint before admitting a result. Support for outer toolchain envelopes follows #1254
and #1259.

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

## Resolved plan inspection (#1256)

`ycxx show app` reports the targets and facts the planner reached. Each source,
include directory, dependency, tool, argument and output has an origin in
`build.ycxx`. A generated source shows its producer and output name. A glob
match shows the expression that selected it. The source format returns the
original definition. The JSON format is a versioned snapshot, and the HTML
format can be opened without a server.

Save a snapshot with `ycxx show app --format=json > before.json`. Compare it
with this checkout using `ycxx show app --against before.json`. The comparison
uses fact kind, owner and name for identity and resolved values for equality.
Locations and descriptions explain changes without causing changes when lines
move. Ordered requirements use their position for identity. A new glob match
is visible even when the manifest text is identical.

Inspection reuses the build definition resolver and lists directory entries.
It reads recently admitted rule expansions and verifies the package and declared
read observations. A rule with no current expansion appears as unresolved.
Source output always reads the authored definition directly. Inspection runs no
tools and reports generator inputs and tools by their planned paths.

## Native rule packages (#1257)

A package owns its definition and exports a host rule plugin target. A workspace
imports that target under a local alias.

```text
use plugin("tools/wire-rules/rules") as wire

wire.generator "wire_files" {
    schema = "schema.txt"
    reads = ["settings.txt"]
}
```

```text
# tools/wire-rules/build.ycxx
host_action "rules" {
    tool = cxx()
    args = ["-fPIC", "-shared", "-Iinputs/sdk",
            input("source"), "-o", output("plugin")]
    inputs = { source = "rule.cc" }
    outputs = { plugin = "rules.so" }
}
```

The planner returns a request when it reaches a custom declaration. The
coordinator builds the package using the selected host C++ toolchain, stages
the resulting shared library, and invokes a worker. Both steps use the
declared action runner. The package definition declares the bootstrap host
action and its shared-library output directly. The action gets the versioned
SDK header through its staged inputs.

`cxx()` tries the current ycxx driver, then looks for `c++`, `clang++`, and `g++`
on the invoking client's `PATH`. It stops at the first successful compile and
link. The trace records each attempted compiler, and unchanged requests reuse
the winning package action. Use `tool = cxx("clang++")` or an absolute path inside
`cxx(...)` to pin the compiler. A pin tries one compiler. `native_args` supplies
extra flags for the native invocation. A successful compile followed by a load
or rule error reports that error from the selected package.

The worker loads the library in its own process and passes a versioned C ABI
record to its entry point. The SDK exposes attributes as strings and declared
file observations as byte ranges. It also supplies the normalized argument
record for rules that pass structured fields on to a primitive. Missing files
and empty files are different observations. A rule emits one native
executable, native library, or external action declaration. The workspace
planner validates that primitive and supplies the compiler and scheduler.
A worker failure produces diagnostics
before the coordinator can admit a successful result.

The bootstrap action key includes the SDK, source and header inputs, host
compiler identity, and build arguments. The worker action key includes the
library bytes, invocation, and declared observations. Prepared declarations
are revalidated against the current inputs. Equivalent expanded generator
actions keep their own keys when the shared library changes. The worker
process bounds crash effects, and cacheable rules use the SDK for their
workspace observations.

`cpp` is a conventional import alias. `use plugin("tools/other-cpp/rules") as cpp`
binds the same declarations to a workspace package. The standard source
package compiles on demand through the same host-action bootstrap and
registers its rules through the same worker contract. The kernel supplies
native compiler queries and external actions to either provider. The
standard package also provides the unqualified `generate` rule.

## Compiler capability observations

```sh
ycxx build app --probe-toolchain --trace-engine=interactive
ycxx show app --format=html > build-plan.html
```

The probe request measures the compiler that successfully built each demanded
`cxx()` package. It uses the same interface for native ycxx, PATH fallback, and a
pinned host compiler. Ordinary builds measure the probes their rules declare. The inspector reads
admitted observations and checks that their profile and inputs still match.

A package can set `probe_flags = ["-std=c++23"]` to measure features in a
selected mode while retaining separate exact-mode probes. Use an absolute
`--sysroot=` path or a configured target flag to describe those choices. The
probe sees the package's declared environment. Optional `probe_inputs` names
additional local files or directories for a wrapper or a custom toolchain.
Relative `probe_inputs` paths are relative to the package definition. The
flags control the probe invocation and the package's `args` control its build.

Each named observation has a phase and a witness. The matrix measures the
unconfigured driver target, exact C++ modes, header inclusion, representative
features, configured and named cross-target objects, hosted and freestanding
links, a hosted executable run, and shared-library compile, link and load. Linked artifact checks
validate the architecture, format and kind. The load action runs only when
the shared artifact matches the host target and calls the test entry point.

The result is `supported`, `unsupported` or `indeterminate`, with the
available evidence. A compiler rejection and a cancelled, timed-out or
unverifiable result are different observations. A time-budget result remains
an observation of that profile. A launch or loader setup failure can retry.
The key includes the versioned
probe recipe, compiler identity, profile, declared environment, and the
observed compiler, include and library search inputs. The coordinator stages
commands outside the query engine and records typed results from an unchanged
input sample. Repeating the request on the same resident profile reuses those
results. [#1289](https://github.com/yrashk/ycxx/issues/1289)

## Typed capability headers

```text
use plugin("builtin:capabilities") as capabilities

capabilities.header "profile" {
    namespace = "project::compiler"
    filename = "compiler_profile.hh"
    capabilities = {
        filesystem = probe("tools/host/rules", "header.filesystem")
        native_objects = probe("builtin:cpp", "object.default")
    }
}

cpp.executable "app" {
    sources = ["main.cc"]
    generated_headers = [artifact("profile", "header")]
}
```

Each `probe(package, id)` demands a named observation from the compiler that
builds that package. A workspace package is addressed by its relative target
path. `builtin:cpp` selects the shipped C++ package. The optional
`builtin:capabilities` package is built and invoked through the ordinary rule
worker. A regular build measures the explicitly demanded probes. The
`--probe-toolchain` flag still requests the full matrix for the selected packages.

The rule worker receives a borrowed array of names and typed support states.
Probe identity, profile, and diagnostics stay with the engine and inspection.
Equivalent projected states keep the worker and generated artifact reusable
across a reprobe. The header package validates names, emits facts in name order,
and exports the header as a named artifact. The generated enum preserves
`supported`, `unsupported` and `indeterminate`. Its `available<State>` variable
template is a C++ constant expression usable in `if constexpr` and constraints.

The trace shows the probes and their named projections. Read-only inspection
revalidates the observation inputs before showing a cached expansion and its
probe and compiler provenance. Watch reobserves both the package and its
declared compiler inputs. [#1290](https://github.com/yrashk/ycxx/issues/1290)

## Package-defined observations

A provider can export a support state for a tool it knows how to check.

```text
use plugin("tools/codec/rules") as codec
use plugin("builtin:capabilities") as capabilities

codec.profile "wire" {
    tool = "/opt/codec/bin/check"
    reads = ["profiles/wire.cfg"]
}

capabilities.header "config" {
    capabilities = { fast = value("wire", "fast-mode") }
}
```

A referenced rule emits an `action.observe` declaration with one `export`
name, a tool, optional arguments, staged inputs and environment. The coordinator
runs the declared witness outside the query epoch. For a normal exit, it gives
the same isolated worker generation the named export, exit status and bounded
stdout and stderr. The optional SDK classifier supplies a single support state.
A normal nonzero exit can support a classification. Setup failures, signals,
timeouts, oversized evidence and invalid classifier replies fail the build
without admitting a typed value. Declared inputs and the provider are observed
even on a failed attempt.

The result has a stable address made from the workspace, producer target and
export name. The receipt tracks the action and provider generation separately.
The consumer's argument query fetches the export and receives only its typed
state. A receipt change with an equal state cuts off downstream work on the
same query cell. Read-only inspection validates the provider and witness inputs
before reading admitted provenance. Watch reobserves those same dependencies.
Existing `probe(package, id)` bindings continue to select a compiler observation
from the shipped catalogue.

The independent Astra design review identified the stable export address and
separate receipt dependencies as the basis for semantic cutoff. [#1257](https://github.com/yrashk/ycxx/issues/1257)
