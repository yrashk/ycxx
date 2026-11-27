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

### Generated static archives (#1339)

A C++ library can export an ordered list of static archives from named
generator outputs. An executable can also declare its own list.

```text
cpp.library "codec" {
    public_link_inputs = [artifact("codec_archive", "archive")]
}

cpp.executable "app" {
    sources = ["main.cc"]
    deps = ["codec"]
}
```

The standard C++ rule forwards these fields in the declaration it emits.
The workspace planner resolves the artifact edges and the coordinator uses
the existing action receipts to supply the archive bytes to the native linker.
All source objects precede the archives. The executable's `link_inputs` come
first, followed by library inputs. A library precedes its dependencies and a
shared dependency follows all its consumers. Among eligible libraries, the
planner uses the order in which the declared dependency lists first reach
them. Each library contributes its list once. Repeated artifact references in
a list stay at their declared positions.

The link uses indexed GNU archives on x86-64 Linux and Darwin archives on
arm64 macOS. Linux archive links require objects without ELF COMDAT groups.
The existing linker coverage is tracked in [#388](https://github.com/yrashk/ycxx/issues/388).

`ycxx show` reports the resolved archive sequence as `link-input` facts. A
transitive entry identifies the library that declared it and points to its
original artifact reference in `build.ycxx`.

### A Make-based C library (#1341)

The [foreign Make example](../../examples/project-build/foreign-make) uses a
source-built C++ rule package to read a declared file inventory. Each entry
names a file relative to the Makefile's directory. The rule emits one Make
action that stages the Makefile and the listed files in the same layout. The
`archive` output is an ordinary generated artifact inherited through
`public_link_inputs`. Read-only inspection shows the staged paths, the Make
command, and the library that exports the archive.

The rule declares its compiler and archiver as `tool_inputs`. Changes to
either executable can rerun Make while equal archive bytes reuse the native
link. [#1343](https://github.com/yrashk/ycxx/issues/1343)

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

Generators and witnesses can name helper executables through a record such as
`tool_inputs = { cc = "/usr/bin/cc", ar = "/usr/bin/ar" }`. Each helper's name,
declared absolute path, canonical pathname, and executable content fingerprint
enter the ordinary action receipt identity. The declaration observes the
helper at its host path. Pass a helper path through the declared arguments
or environment when the action needs it. Watch reobserves both spellings.
`ycxx show` reports the canonical path and fingerprint of a current helper.
An unavailable generator helper has a diagnostic fact, and a stale witness
snapshot is reported as a partial plan until the next successful build.
[#1343](https://github.com/yrashk/ycxx/issues/1343)
[#1526](https://github.com/yrashk/ycxx/issues/1526)

Keep subprocess execution outside ordinary reevaluation. A query returns
pending action demands to the build coordinator. The coordinator deduplicates
and executes them, admits complete receipts as tracked inputs at quiescence,
and requests the root again under the same outer trace. Waiting inside the
epoch for a receipt that requires the epoch to end would deadlock. Engine
verification rechecks queries using receipts without launching tools.
Publication uses a completed request's generation and can restore removed
output files from cached values.

### Staged action tools (#1505)

A rule package can emit an `action.generate` whose tool is a named input
selected from a producer's artifact. In the staged tool example, the
`runner` action copies an executable script while preserving its mode.
The consumer runs the staged copy to write a generated header.

```text
action.generate "config" {
    tool = input("runner")
    args = [output("header")]
    inputs = { runner = artifact("runner", "runner") }
    outputs = { header = "generated_config.hh" }
}
```

The selector must name an artifact-backed input with the executable bit
set. The coordinator uses the ordinary producer receipt to stage that input,
and the consumer action identity includes the staged path, bytes, and
executable mode. When the producer runs again and returns an equivalent
selected output, the consumer can reuse its receipt. Watch follows the
producer's declared inputs. In `ycxx show app --format=html`, the tool
fact shows the planned staged path and selector, and **Target dependencies**
links the consumer to its producer. [Runnable example](../../examples/project-build/staged-tool/README.md)

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

Project builds accept the standalone compiler's native target triples (#1345).
Pass `--target=<triple>` to `build`, `build --watch`, and `show` to select the
same variant. Explicit variants keep their executable, runtime assets, virtual
generated inputs and rule snapshot under `.ycxx/targets/<triple>/`. The host
default uses the original paths directly under `.ycxx/`. Rule packages and
external actions run on the host and share receipts when their declared inputs
match. An external action that supplies a linker input must produce an archive
for the selected target.

The standard rule package's `cpp.host_executable` rule exposes one named
`executable` artifact (#1559). Its resident native build uses disk sources,
ordinary includes, and the canonical host target. The selected app target
has its own compilation request. A generator stages the linked bytes and
executable mode in an ordinary input, which can also be used in
`tool = input("name")`. The build trace includes the host compilation and
link. The action receipt reuses an equal staged image.

The coordinator verifies the engine revision before launching an action
that depends on a host build and again before admitting its receipt. Watch
observes the source and include reads through the same file table as native
compilation. Host executables are available to ordinary generator recipes
and staged witness tools. Ordinary generator chains can also run them
upstream of a generated planning input.
[Native host tool example](../../examples/project-build/native-host-tool/README.md)

### Libraries for native host tools (#1576)

```text
cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    deps = ["shared"]
}
```

Host executable dependencies are disk-backed `cpp.library` declarations.
Each executable collects its own reachable sources and public include
directories. In a diamond, both dependency paths remain visible in the
plan and the shared library contributes its sources once per executable.
The same library declaration can supply both the host scanner and a
foreign-target app. The scanner's source and include observations cover its
library translation units, so edits can wake watch and refresh the staged
host image.
[Shared library diamond example](../../examples/project-build/host-cxx-libraries/README.md)

### Generated headers on a native host executable (#1579)

```text
cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    generated_headers = [artifact("scanner_config", "header")]
}
```

The declared producer and its ordinary predecessors run before the host
compile. That action chain must be independent of project-built native host
tools. Generators keep the requesting plan's argument selectors. C++ still
compiles the executable for the host.

Each admitted producer directory exposes the declared output names and
their exact bytes through the executable's own include configuration.
The immutable snapshot uses stable logical paths under `.ycxx/host/virtual`.
Disk sources and real include directories use ordinary disk lookup.
The selected app has its own generated include view.
Equal visible files preserve the host compile, and equal linked executable
bytes preserve consuming generator receipts. The coordinator shares staged
producer outputs across ordinary builds, planning inputs, and witnesses.
[Generated host header example](../../examples/project-build/host-generated-headers/README.md)

The same producer restriction applies to artifacts in a host executable's
`sources` list. Such a translation unit reads its main file from the host
snapshot, and quoted includes can find declared siblings in its logical
directory. Disk sources and generated artifacts can be mixed in authored
order. Diagnostics refer to the logical source path and use the snapshot
bytes. Planning and witness receipts observe real disk includes, including
misses, and can persist an empty disk observation when the tool is generated.
[Generated host source example](../../examples/project-build/host-generated-sources/README.md)

### Generated headers in native host library dependencies (#1588)

A disk-backed `cpp.library` can declare `generated_headers` when it is
linked into a native host executable. The host tool stages the reachable
ordinary producers once. Each library unit's immutable file view exposes
the declared siblings in its own and inherited generated include
directories. An out-of-scope host virtual path resolves as a miss. Edits
to a sibling's generated files preserve the unit's compile identity,
and real disk includes continue to drive watch.

The same library declaration can serve the selected target, with the
target's generated include paths and compile identity. Inspection shows
both resolved include views and links them to the authored header binding.
[Generated host library header example](../../examples/project-build/host-library-generated-headers/README.md)

### Generated sources in native host library dependencies (#1600)

A host-linked library can put an artifact reference in `sources`
alongside disk paths and globs. The library's generated main sees the
output siblings of its producer at their host logical paths. The main's
directory participates in quoted include lookup through the includer's
path. Adding the source leaves the library's public include search
unchanged. Other units can see that producer's directory when a
generated header binding makes it visible to them.

Both the selected target and the host tool can compile the shared
library. The planner stages the ordinary source producers before the
native host compile and reports the two logical source paths against
one authored binding. Watch and early planning continue to observe the
real includes used by each generated main.
[Generated host library source example](../../examples/project-build/host-library-generated-sources/README.md)

### Direct generated archives for native host tools (#1616)

```text
cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    link_inputs = [artifact("scanner_archive", "archive")]
}
```

The coordinator stages the ordinary archive producer chain together with
generated host sources and headers before registering the host build. The
archive chain must be independent of project-built host tools. Its actions
keep the selected target's argument selectors, and the native linker reads
archives in the host format. The host's source objects precede the archive
operands. Authored order and repeated references are preserved.

The archive names and exact bytes enter the host program key. Archive-only
producers stay outside the preprocessor's host file view, so an archive
edit can relink without recompiling the host sources. Equal host images
reuse consumer receipts. Before admitting a result against a revised
plan, the coordinator can compare the staged inputs with existing receipts
without running their producers. The plan shows each archive operand's host
path, output binding, and manifest location.
[Host archive example](../../examples/project-build/host-static-archive-inputs/README.md)

### Archives inherited by native host tools (#1622)

```text
cpp.library "scanner_codec" {
    sources = ["codec.cc"]
    public_link_inputs = [artifact("codec_archive", "archive")]
}

cpp.host_executable "scanner" {
    sources = ["scanner.cc"]
    deps = ["scanner_codec"]
}
```

The host link puts direct archives before inherited archives. Each library
contributes its authored operands after all of its reachable consumers,
so a shared library contributes once in a diamond. Explicit repeats retain
their positions and use the same staged archive bytes.

The host tool and selected executable can inherit from the same library.
Each authored reference selects one ordinary producer receipt. Both links
read those bytes and check them against their own object formats. A project
that needs separate formats can bind distinct outputs through separate
library declarations. The plan shows each link's resolved operand sequence
and paths alongside the shared authored edge.
[Inherited host archive example](../../examples/project-build/host-library-static-archive-inputs/README.md)

Project generators and rule witnesses accept `target_triple()` (#1347)
and `host_triple()` (#1534) in `args`. Each selector supplies one canonical
argv word. `target_triple()` resolves to the selected compiler target and
defaults to the host. `host_triple()` resolves to the host across all target
selections. The resolved words enter the action key, so identical argv and
inputs can reuse a receipt across target selections and equivalent literals.
Typed witness values have a separate identity per target triple. Read-only
inspection shows each argument and its manifest location. Value traces show
the resolved argv position and selector. [Target-aware example](../../examples/project-build/target-aware-action/README.md)

The same selectors can be used as project generator and witness
`environment` values (#1540). The tool receives the resolved strings.
Receipt identity includes those strings, so equal literal values can
reuse a receipt. Plan inspection shows the resolved environment entry
and the selector that supplied it. [Environment walkthrough](../../examples/project-build/target-aware-action/README.md#pass-the-triples-in-the-action-environment)

`host_action` package recipes build the rule package for the host and
accept string values in `environment`.

Named-module discovery reads the preprocessor's marked module and import
directives before the native front end runs (#1349). It uses the registered
unit's target, include paths, and generated-file view. Each unit contributes
its declaration and named imports to a resident graph. The graph's value omits
source locations, so moving a directive can update its provenance without
changing the graph. Discovery reports dependency facts. Named-module
compilation and BMI support are tracked in #319.

Read-only inspection rescans available sources with the same configuration
once their rules resolve. A scan that consulted a generated file, including
a negative include probe, is marked pending in the local plan. The build
trace can show that unit once its actions provide the virtual inputs.

Provider resolution connects written named imports to reachable interface
units and named partitions for the selected target (#1353). A primary
implementation unit does not declare a provider. Repeated registrations of
one translation unit count as one provider. Imports with no matching
declaration are external or unresolved. Provider conflicts are visible even
without an import, and only unambiguous edges enter cycle analysis. The
reported cycles list the members of each cyclic component.

The resident resolver depends on the location-free module graph. A separate
report pairs its edges with physical C++ source locations. Read-only plans
use the same resolver. When a scan is pending, unique or missing matches
remain provisional and cycles describe the known graph. The compiler work
that uses these dependency facts for module semantics and BMIs is tracked in
#319.

Declared host and target toolchains and broader project layouts grow coverage
in #1259. Package acquisition and version solving sit outside the build.

## Shader bundles and runtime assets (#1258)

```text
use plugin("tools/shaders/rules") as shaders

shaders.bundle "scene" {
    inventory = "shaders/inventory.txt"
    reads = ["shaders/inventory.txt"]
    compiler = "/usr/bin/glslc"
    python = "/usr/bin/python3"
}

cpp.executable "app" {
    sources = ["main.cc"]
    runtime_assets = [artifact("scene", "bundle")]
}
```

A package can emit a bounded definition containing a public `action.generate`
with the requested target name and private `action.generate` dependencies.
Private names are local to that expansion. The existing worker protocol carries
the whole definition. An action's named input can select a named output through
`artifact(target, output)`. The planner checks the reachable graph and rejects
cycles. The engine prepares downstream actions when their selected input
receipts arrive. Action identity uses the concrete staged bytes and modes,
the recipe, and the tool identity. A syntactically different expansion can
therefore reuse an equivalent action. Plan inspection shows the artifact edges.

The shader package stages one source per compile. The packer takes the named
outputs and produces the bundle. A native target's `runtime_assets` are
published under `.ycxx/assets/<target>/<filename>`. The selected assets are
independent of the native compiler inputs. The coordinator publishes after
the actions and native build succeed. A failed compile leaves the last
published bundle in place. A missing published bundle can be restored from
the resident receipts.

The Astra architecture review identified named receipt bytes, private action
names, and a separate runtime publication path as the seams for this slice.
The same artifact edges feed the structured reflection witness in
[#1258](https://github.com/yrashk/ycxx/issues/1258).

### Runtime asset generations (#1329)

The public path remains `.ycxx/assets/<target>/<filename>`. A changed asset
set is staged as one sibling directory. Linux and macOS exchange the live
directory with the finished candidate in a single filesystem operation.
The first publication installs the candidate at the previously unused path.
An unchanged set with identical bytes and modes keeps its published files.
The directory switch is atomic for path resolution. A reader performing
multiple independent opens can span a successful switch.

The candidate carries the names that this target owns. The marker is linked
to a private, uniquely named file under `.ycxx/asset-manifests` so an ordinary
output or an unrelated file with similar contents remains distinguishable.
Each successful plan replaces its owned names as a set, including a plan
with no runtime asset references. Unrelated directory entries carry forward.
A new declaration that collides with an unrelated entry fails once the target
has an ownership record. On upgrading an older output directory, current
declarations adopt their destinations and unclaimed names carry forward.

The publisher stages the asset candidate and the executable before installing
either. It retains the displaced executable until the final asset switch
succeeds and restores it if the switch fails. A build that fails before the
switch leaves the live asset directory available. Watch retries when its
observed source or plan inputs change. The independent Astra review identified
directory exchange, a per-generation ownership record, and a final fallible
switch as the portable boundaries of the transaction.

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

### Resolved dependencies (#1453)

`ycxx show app --format=json` includes `dependency_graph` in its schema-1
snapshot. Each entry in `edges` records a `consumer` and its `producer`. The
`usage` is the field that uses the dependency. A `deps` edge names a library.
The fields `sources`, `generated_headers`, `link_inputs`,
`public_link_inputs`, `runtime_assets`, `inputs`, and `plan_inputs` bind
to an artifact's named `output`. The `inputs` and `plan_inputs` edges also
record the consumer's input name as `binding`.

The `location` identifies the use in `build.ycxx` by path, one-based line, and
one-based byte column. A use emitted by a rule points to its workspace
declaration when the rule introduced the value. The report orders edges by
consumer and source location. If a rule needs a current expansion, the graph
reports `complete: false` with the edges resolved so far. A complete plan can
have an empty edge list.

`ycxx show app --format=html > plan.html` displays those edges in **Dependencies**
with links to their use sites in the displayed manifest. An HTML comparison
also shows the current plan's dependencies. **Plan changes** compares
semantic facts. The `link-input` facts describe the executable's effective
archive order. The read-only report can identify an artifact before its
bytes exist.

### Typed value bindings (#1477)

In `examples/project-build/package-probe`, the `config` header binds `fast`
to the `fast-mode` export of the `wire` target.

```text
capabilities.header "config" {
    namespace = "example::codec"
    filename = "codec_config.hh"
    capabilities = { fast = value("wire", "fast-mode") }
}
```

From that example directory, run `ycxx build app` and then
`ycxx show app --format=html > plan.html`. **Value bindings** shows `config`
as the consumer, `capabilities` as the usage, `fast` as the binding, and
`wire` as the producer with export `fast-mode`. The declaration link opens
the `value(...)` reference in `build.ycxx`. Structured `facts` bindings use
the same report section.

Schema-1 JSON reports these references in `dependency_graph.value_edges`
with `consumer`, `usage`, `binding`, `producer`, `export_name`, and
`location`. A complete plan with no value bindings has an empty array.
An incomplete plan reports the bindings resolved so far with
`dependency_graph.complete: false`. The HTML comparison shows the current
plan's **Value bindings** alongside **Dependency changes**.

### Dependency comparisons (#1464)

`ycxx show app --against before.json --format=json` includes
`dependency_changes` when both plans have complete dependency graphs. The
comparison groups occurrences by consumer, usage, producer, output, and
binding. Each group with different counts has `before` and `after` arrays
of the original edge records, including every source site on each side.
An added dependency has an empty `before` array, and a removed dependency
has an empty `after` array.

Source locations explain where dependencies were used. Moving a reference
with the same identity and count produces an empty `dependency_changes`
array. **Plan changes** reports changes in the effective archive order
through the `link-input` facts. Older schema-1 snapshots without a
dependency graph can still compare plan facts.

Run `ycxx show app --against before.json --format=html > changes.html` to
read **Dependency changes** alongside the current **Dependencies**. The
table shows before and after counts and source sites. After links open
the relevant lines in the current manifest. Before sites describe the
saved plan. A comparison with equal counts shows **No dependency changes.**

### Value binding comparisons (#1493)

From the package-probe example, build the plan and save a baseline.

```sh
ycxx build app
ycxx show app --format=json > before.json
```

Rename the producer from `wire` to `wire_alt` in its declaration and its
`value("wire", "fast-mode")` reference in `build.ycxx`. Make the same rename
in the observer declaration and target check in `rules/codec.cc`. Build the
edited plan and open the comparison report.

```sh
ycxx build app
ycxx show app --against before.json --format=html > changes.html
```

**Value binding changes** shows the removed `config` binding to `wire` and the
added binding to `wire_alt`, with before and after counts. Saved sites give
the original coordinates. Current links open the reference in the displayed
manifest. **Value bindings** shows all resolved bindings in the current plan.

The same comparison with `--format=json` reports `value_binding_changes`.
A binding's identity is its consumer, usage, local binding, producer, and
export. Each identity with different occurrence counts includes all its
`before` and `after` edge records. Moving a reference for the same binding
preserves its count. When counts match, JSON returns an empty array and HTML
says **No value binding changes.**

The binding comparison requires complete graphs with `value_edges` in both
snapshots. Older schema-1 baselines still support the comparisons for which
they have data.

### Target dependency navigation (#1499)

From `examples/project-build/generated-archive`, build the example and
save its HTML plan.

```sh
ycxx build app
ycxx show app --format=html > plan.html
```

Open **Target dependencies** and start at `app`. Follow `score`, `bonus`,
and `bonus_archive` to see how the generated archive reaches the executable.
The `public_link_inputs` reference on `bonus` links to both the archive
producer card and the use site in `build.ycxx`.

The same navigation follows typed bindings in the shader reflection example.
From `app`, follow `scene_bindings` and its `facts` binding to
`fragment_reflection`.

```text
shaders.bindings "scene_bindings" {
    facts = { layout = value("fragment_reflection", "layout") }
}
```

Each target has one card, so all occurrences of a shared producer lead to
the same destination. A partial plan shows the resolved paths and marks
the navigation incomplete. In an HTML comparison, these links navigate
the current plan. The comparison tables carry the saved occurrence sites.

### Interactive plan graph (#1604)

From `examples/project-build/host-library-generated-sources`, build the
arm64 Darwin variant and save its HTML plan.

```sh
ycxx build app --target=arm64-apple-darwin
ycxx show app --target=arm64-apple-darwin --format=html > plan.html
```

Open **Graph** in `plan.html`. It starts at `app` with its immediate
dependencies, `config` and `schema`. Select a node to show or hide its
dependencies and dependents. Expand `config` to find `scanner`, then show
the scanner's dependencies. The scanner and the app point to the same
`schema` node.

Select `schema` to inspect its generated `wire_score.cc` paths for the host
and selected target. The details list each authored dependency's usage and
any named output or binding. Follow the location link for the `wire_source`
edge to open the source binding in `build.ycxx`.

The graph works offline in the saved report. An incomplete plan shows the
resolved portion and marks it incomplete. The static **Target dependencies**
cards and dependency tables remain available alongside the graph.
[Host library generated sources example](../../examples/project-build/host-library-generated-sources/README.md)

### Declared target catalog (#1392)

Run `ycxx show --targets` from the workspace directory to see the target names
you can pass to a per-target command. The catalog parses `build.ycxx` in
that directory. It accepts a regular UTF-8 file or a symlink to one (#1441).
Names are sorted in byte order and retain any embedded slashes.
The catalog shows rule kinds as declared, including custom rule aliases.
The read-only command works before the first build and runs independently of
the planner, build cache, and daemon.

Text is the default. With `--format=json`, the object has `schema` set to `1`
and a `targets` array. Each record has `name`, `kind`, and a `location` with
`path` set to `build.ycxx` and a one-based `line` and byte `column` for the
declaration. With `--format=html`, the catalog links each entry to that line
in the displayed manifest.

Catalog mode accepts one `--targets` flag and a text, JSON, or HTML format.
A positional target name, `--target`, `--against`, or `--format=source` is a
usage error in this mode.

### Structured named-module graph (#1363)

`ycxx show app --format=json` includes `module_graph` for a resolved plan under
a supported native configuration. A plan with no C++ translation units has an
empty graph. The field is absent while rule expansion is unresolved or when no
native configuration is available.
The graph reports `native_target` as the selected target triple. `complete`
indicates whether every unit scan is complete. The graph contains `units`,
`imports`, `conflicts`, and `cycles` arrays.

- A unit has `id`, `source`, `scan`, and `declarations`. A translation unit
  registered more than once appears once. Different configurations of the same
  source have distinct IDs. `scan` is `complete`, `generated_inputs_unavailable`,
  or `preprocessing_incomplete`. The optional `configuration` records the
  logical `source` path and ordered `include_dirs`.
- A declaration has `directive_index`, `kind` (`interface_unit` or
  `implementation_unit`), `name`, and `location`. An import adds `unit_id`,
  `status`, and `candidates` and has kind `import` or `exported_import`.
- Each import or conflict candidate identifies a declaration with
  `unit_id` and `directive_index`. Unit IDs are zero-based integers scoped to
  one snapshot. IDs and declaration indexes can be sparse. Look up declarations
  by their `directive_index` field.
- Conflicts have a `name` and `candidates`, including conflicts with no matching
  import. Each cycle is an array of unit IDs in one cyclic component of the
  known graph.

| Import status | Meaning |
| --- | --- |
| `resolved` | One candidate in a complete graph. |
| `provisional` | One known candidate in an incomplete graph. |
| `ambiguous` | Multiple known candidates. |
| `pending` | No known candidate in an incomplete graph. |
| `unresolved_partition` | No candidate for a partition import in a complete graph. |
| `external_or_unresolved` | No candidate for a named module import in a complete graph. |

Each `location` has `path`, `line`, and `column` for the first keyword of the
directive. Lines and byte columns are one-based and refer to the physical file.
Paths within the workspace are relative to it. Moving a directive refreshes
its provenance while preserving its dependencies. Schema 1 retains `facts` and
`show --against` compares those semantic facts, so earlier schema-1 snapshots
still work. The module-provider example joins these records to compare both
target variants. [Module-provider example](../../examples/project-build/module-providers/README.md)

### Module graph in HTML (#1378)

`ycxx show app --format=html > plan.html` renders the same optional graph as a
**Module graph** section in the standalone report. Each configured translation
unit has a panel for its declarations and imports, with physical source
locations and import statuses. Candidate links jump to the exact declarations
in the report. Incomplete scan notes identify the affected units. Conflicts
link to their candidate declarations and each cyclic component links to its units.

With `--against`, **Plan changes** compares the saved snapshot's semantic
facts with the current plan. **Module graph** follows that comparison and shows
the current plan's graph. A resolved plan with a supported native configuration
and zero C++ translation units shows an empty graph. The section is omitted
while rule expansion is unresolved or when no native configuration is available.

### Module graph comparisons (#1383)

`ycxx show app --against before.json --format=json` includes `module_changes`
when both snapshots contain a module graph. Its `units`, `imports`, `conflicts`,
and `cycles` arrays contain records with `change` (`added`, `removed`, or
`changed`) and the applicable `before` and `after` records from the original
graphs. Resolve candidate references and cycle members in their respective
snapshots. If graph completeness changes, `completeness` carries `change`
(`changed`) and the `before` and `after` booleans. In an incomplete graph,
removed entries can reflect a loss of scan coverage.

Units match by `configuration.source` and the ordered
`configuration.include_dirs`. The target triple may differ between snapshots.
Paths inside the workspace are relative to it. Per-target virtual source and
include paths use the common `.ycxx/virtual` root for matching.
Unit changes cover scan state and named declarations. Repeated imports of the
same kind and name in a unit pair by occurrence order. Import changes cover
resolution status and the set of candidate declarations. Conflicts match by
module name and cycles by their set of configured units. A body-only edit or a
moved directive produces empty change arrays when the observed graph facts agree.

For an older graph with units that lack `configuration`, unambiguous source
paths allow source-only matching. The result sets `matching` to
`legacy_source_only`. Ambiguous source matches produce empty change arrays and
set `unavailable` to `ambiguous_legacy_configuration`. The plan's `changes`
array contains the semantic fact comparison in either case. Snapshots saved
without a graph still produce the original fact comparison.

### Module graph comparisons in HTML (#1390)

`ycxx show app --against before.json --format=html > changes.html` shows
**Module changes** above **Plan changes** and the current **Module graph**
when both plans have graphs. It uses the same semantic comparison as JSON.
Before entries show sites from the saved snapshot. After entries link to the
current imports, provider declarations, and translation units. The comparison
also shows scan completeness, changes to individual unit scans, conflicts,
and cycles. A partial scan can change which providers are observable.

An older graph with unambiguous unit paths can compare by source path. The
report identifies that fallback and explains when repeated source paths make a
comparison unavailable. An available comparison with equal semantic results
shows **No semantic module changes.** A schema-1 snapshot without a graph
still shows the fact comparison and the current graph when available.
Inspection is read-only. Named-module compilation and BMI support are tracked
in [#319](https://github.com/yrashk/ycxx/issues/319).

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

### Generated planning inputs (#1335)

```text
shaders.bundle "scene" {
    inventory = "inventory"
    plan_inputs = { inventory = artifact("shader_inventory", "list") }
}
```

A workspace rule can bind `plan_inputs` names to public generator outputs.
An input has a single logical name and cannot share a name with an ordinary
`reads` entry. The isolated worker uses `read` with that name and receives
the selected receipt's bytes. The artifact is available to planning before
the rule emits its declarations. The same generator and receipt serve any
ordinary artifact consumers. The planner rejects cycles across planning,
artifact and value edges.

The worker's request depends on the named input bytes. When a generator
runs and produces equal bytes, the existing worker result stays reusable.
For inspection and watch, the admitted expansion records the selected output
and its producer graph's recipe, tool and declared input identity separately.
`ycxx show` verifies the captured graph before reporting the provenance of an
expansion. A stale graph becomes unresolved until a build supplies a current
receipt. Watch follows the generator graph even if it fails or returns an
invalid inventory. Publication keeps the previous assets until planning,
the ordinary actions and the native build succeed.

### Host tools upstream of planning inputs (#1574)

The native inventory example builds `scanner` for the host and runs it
in a generator. A copy action exposes the selected output to the `config`
rule through `plan_inputs = { inventory = artifact("inventory", "list") }`.
The worker reads those bytes and emits a generator for a `constexpr` header.
The app can target a different platform from the scanner.

The coordinator observes the reachable host tools together and records
source and include lookups, including misses. It revalidates the worker's
selected bytes before admitting the expansion. Equal bytes preserve the
worker receipt and update the producer provenance. The coordinator keeps
planning and witness file evidence separately. When the same rule also
classifies a witness, both phases must agree on the inputs they share.
Read-only inspection validates saved evidence, and watch retains failed
lookups so creating a missing include can wake the build.
[Host planning input example](../../examples/project-build/host-planning-inputs/README.md)

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
the same isolated worker generation the named export, exit status, and up to
64 KiB each of stdout and stderr. The optional SDK classifier supplies a single support state.
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

### Staged witness tools (#1515)

An `action.observe` can select its tool through an executable artifact input.
In the staged-witness example the producer copies `analyzer.sh` as its named
`runner` output while preserving its executable mode.

```text
action.observe "observed" {
    tool = input("analyzer")
    inputs = { analyzer = artifact("runner", "runner") }
    export = "score"
    schema = "example.score.v1"
}
```

The coordinator stages the producer receipt and invokes the selected copy
as the witness. Its bytes and executable mode are part of the witness action
key. The classifier turns the output into a typed `score` row, which the
header rule consumes with `facts = { score = value("observed", "score") }`.
An edit that preserves the classified score can reuse header generation
and native compilation. Build the example with `ycxx build app` and inspect
the artifact and value links with `ycxx show app --format=html`.
[Runnable example](../../examples/project-build/staged-witness/README.md)

A witness can bind `tool = input("name")` directly to the `executable`
artifact of a `cpp.host_executable` (#1566). The resident host query captures
its linked image and the paths and content fingerprints of its source
and include reads in one revision. The observations include missing
include candidates so a newly preferred header can invalidate them.
Read-only inspection checks the saved paths and fingerprints. Watch
retains the lookups from a failed compile so creating a missing header
can wake the build.

The witness receipt uses the staged executable bytes and mode. Edits
that preserve the image refresh the saved file observations and reuse
the receipt. The coordinator checks the image again if its inputs change
while the witness runs. [Native host witness example](../../examples/project-build/native-host-witness/README.md)

The same staged analyzer can call a helper at an absolute host path. Declare
the path in `tool_inputs` to track helper changes as witness inputs. Equal
classified scores still reuse the app. [Helper walkthrough](../../examples/project-build/staged-witness/README.md#track-a-helper-executable)

### Witnesses with host producers (#1571)

A staged witness can select an ordinary generator output whose producer
chain consumes native host executables. The example runs the host
`scanner` in `analyzer_source` to write an executable script. The
`analyzer` action copies that script and preserves its mode. The emitted
`observed` witness selects it through `tool = input("analyzer")` and
`inputs = { analyzer = artifact("analyzer", "executable") }`.
The `config` rule consumes the classified score with
`facts = { score = value("observed", "score") }`.

The coordinator collects the linked images and source and include
observations of all reachable host tools in one current engine revision
before staging the producers. It revalidates the recipe and staged inputs
when files change during generation or classification. Generator and
witness action keys use the concrete staged bytes and executable mode.
Equal selected outputs can therefore reuse the witness receipt, which
stores the exit status, stdout, and stderr. The classifier publishes a
separate typed value. Equal values preserve reuse for downstream consumers
such as the header generator. Read-only inspection and watch reobserve
the reachable host tool inputs.
[Host producer witness example](../../examples/project-build/host-producer-witness/README.md)

### Structured shader reflection

```text
use plugin("rules/package") as shaders

shaders.module "fragment" {
    source = "shaders/triangle.frag"
    compiler = "/usr/bin/glslc"
}

shaders.reflect "fragment_reflection" {
    module = "fragment"
    reflector = "/usr/bin/spirv-cross"
}

shaders.bindings "scene_bindings" {
    facts = { layout = value("fragment_reflection", "layout") }
}

cpp.executable "app" {
    sources = ["main.cc"]
    generated_headers = [artifact("scene_bindings", "header")]
    runtime_assets = [artifact("scene", "bundle")]
}
```

The bundle and reflection rule both select the `spirv` output of `fragment`.
The reflection rule emits `action.observe` with a `schema` and an artifact
input. The coordinator resolves the producer graph and stages the same named
compile receipt that the packer uses. A witness and a generator may depend on
the same action even when the observer needs its output before native planning
can finish. A runtime-only request reaches the packer and its compile actions
without demanding reflection.

The classifier exports the declared schema and a bounded table of named rows
with named fields. Fields are unsigned 32-bit integers or strings. The worker
rejects duplicate names and canonicalizes row and field order. The shader
package defines `shader.descriptors.v2` and emits one row per uniform buffer or
one-dimensional literal `sampler2D` array, with its descriptor set, binding,
kind, and element count. Its bindings rule generates `constexpr` C++ values,
including an enum for descriptor kind. The existing support-state ABI and
`capabilities` bindings continue to use the same export address.

An implementation edit can change the SPIR-V bytes and the witness receipt
while leaving the descriptor table equal. The stable value query then cuts
off propagation to the bindings rule. A descriptor edit reaches the generated
header and the native build. Read-only inspection revalidates the observed
producer graph's inputs and tools before showing the recorded layout. Failed
compilation or reflection leaves the last successful executable and bundle
available to run. Watch keeps the recipe inputs in scope for recovery.
