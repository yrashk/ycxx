# Embedding ycxx

The query engine, object formats, linker, compiler pipeline and project build remain separate
CMake libraries. Each build chooses static or shared components and one C++ standard library.
All components in a build use the same runtime. The CLI's process-wide allocation replacements
are linked only into the CLI and the allocation tests that exercise them.

With the system C++ library, ycxx imports the bootstrap toolchain's `std` module. The execution
provider re-exports the libycxx sender, scope and scheduler implementation built against that
same system C++ library. With libycxx, it re-exports those facilities from libycxx's `std` module.
The standard library chosen to build ycxx and the runtime used by programs ycxx compiles are
independent choices.

The component modules expose C++ interfaces for clients built with the corresponding toolchain
and standard library. An engine client owns its `ycxx::engine::Db`. A compiler client can own a
`ycxx::pipeline::Session`, which in turn owns the values returned from its queries. Copy query
results that must outlive updates to the session. The installed targets and resource paths are
listed in the [embedding instructions](../BUILDING.md#linking-ycxx-into-a-c-application).
