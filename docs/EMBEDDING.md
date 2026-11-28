# Embedding ycxx

Link `ycxx::engine` to use the incremental query engine in your own application.
Link `ycxx::compiler` to supply C++ source, inspect diagnostics, and produce objects or
executables in process. The other targets let you use individual stages.

## Build and consume the SDK

Use the same GCC toolchain and C++ standard library for ycxx and its consumer. The interfaces
are C++ modules and pass C++ types across the library boundary. See the toolchain setup
instructions in [Building ycxx](BUILDING.md#the-compiler).

From a ycxx checkout on x86-64 Linux, build a static SDK that uses GCC's libstdc++.

```sh
git submodule update --init
cmake -S . -B build-sdk -G Ninja \
  -DYCXXC_STDLIB=system -DYCXXC_INSTALL=ON -DYCXXC_BUILD_TESTS=OFF
cmake --build build-sdk
cmake --install build-sdk --prefix "$PWD/install-sdk"
```

Use `YCXXC_BUILD_SHARED_LIBS=ON` to build shared libraries. Use `YCXXC_STDLIB=libycxx`
to select libycxx, the default runtime. Give each configuration its own build directory
and installation prefix. A system build uses libycxx's portable execution provider for
senders, scopes, and scheduling while using GCC's `std` module and libstdc++ for the host.

In your application's `CMakeLists.txt`, select a component. Each example below gives the
component to use and the contents of `main.cpp`.

```cmake
cmake_minimum_required(VERSION 3.30)
project(embed_example LANGUAGES C CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(YCXX_COMPONENT engine CACHE STRING "ycxx component to link")
find_package(ycxx CONFIG REQUIRED COMPONENTS ${YCXX_COMPONENT})

add_executable(embed_example main.cpp)
target_link_libraries(embed_example PRIVATE ycxx::${YCXX_COMPONENT})
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=/path/to/g++ \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/install-sdk -DYCXX_COMPONENT=engine
cmake --build build
./build/embed_example
```

For an in-tree dependency, configure the same GCC toolchain for the parent project,
set the `YCXXC_*` options before adding ycxx, and use
`add_subdirectory(path/to/ycxx ycxx EXCLUDE_FROM_ALL)` in place of `find_package`.
The target names are the same. The repository ships
[build-tree and installed-package examples](../examples/embedding).

## Choose an API

| CMake target | C++ modules | Start here |
| --- | --- | --- |
| `ycxx::engine` | `ycxx.engine` | Own a `ycxx::engine::Db`, register typed inputs with `set`, and evaluate memoized queries with `get`. Queries read dependencies through `Ctx::fetch`. |
| `ycxx::preprocessor` | `ycxx.pp` | Use `pp::Config` and `pp::preprocess_to_text` for expanded text and diagnostics, or `pp::preprocess` for tokens. |
| `ycxx::frontend` | `ycxx.parse` and its dependencies, including `ycxx.ast` and `ycxx.sema` | Compose the parser and per-item semantic queries directly. For source-level diagnostics on a translation unit, start with `pipeline::TuFront` through the compiler target. |
| `ycxx::ir` | `ycxx.ir` | Use `ir::Builder` and `ir::verify` for L1 function bodies. Use `ir::parse` and `ir::print` for the text form. |
| `ycxx::object` | `ycxx.obj`, `ycxx.obj.elf`, `ycxx.obj.macho`, `ycxx.obj.archive` | Work with in-memory objects, read and write x86-64 ELF or arm64 Mach-O objects, or work with archives. |
| `ycxx::linker` | `ycxx.link`, `ycxx.link.elf` | Link prepared in-memory objects and return image bytes. The ELF API accepts an explicit entry symbol. Use the compiler pipeline for its target startup handling. |
| `ycxx::compiler` | `ycxx.pipeline` | Keep a `pipeline::Session` across edits. Use `TuFront` for analysis and diagnostics, `Tu` for objects, and `Link` for programs registered with `set_program`. |
| `ycxx::project_build` | `ycxx.build.inspection`, `ycxx.build.project` | Use `build::show_targets` for the workspace target catalog or `build::show_project` for plan inspection with a rule toolchain. `build::Coordinator` takes a session and host callbacks for project execution. |

## Recompute a query after an edit

Use the `engine` component. The first two reads share a cached result. Setting the input
again with the same key makes the next read recompute its dependent query.

```cpp
import std;
import ycxx.engine;

namespace engine = ycxx::engine;

struct [[=engine::input{engine::Durability::low}]] Number {
  using Key = int;
  using Value = int;
};

struct Twice {
  using Key = int;
  using Value = int;

  static engine::query_task<Value> run(engine::Ctx& ctx, Key key) {
    co_return 2 * ctx.fetch<Number>(key);
  }
};

int main() {
  engine::Db db;
  db.set<Number>(0, 20);
  std::cout << db.get<Twice>(0) << '\n';
  std::cout << db.get<Twice>(0) << '\n';
  db.set<Number>(0, 21);
  std::cout << db.get<Twice>(0) << '\n';
}
```

The output is `40`, `40`, then `42`.

## Preprocess a string

Use the `preprocessor` component. The output includes the expanded declaration
`int answer = 42;`.

```cpp
import std;
import ycxx.pp;

int main() {
  auto& session = ycxx::pp::configure(ycxx::pp::Config{});
  const auto [text, messages] = ycxx::pp::preprocess_to_text(
      session, "#define ANSWER 42\nint answer = ANSWER;\n", "example.cpp");
  for (const auto& message : messages)
    std::cerr << message.text << '\n';
  std::cout << text;
  return std::ranges::any_of(messages, [](const auto& message) { return message.error; });
}
```

## Compile source to an object

Use the `compiler` component. This example compiles a function to an x86-64 ELF
relocatable object and writes `answer.o`. It selects the target explicitly and uses
source that needs no headers.

```cpp
import std;
import ycxx.ast;
import ycxx.obj.elf;
import ycxx.pipeline;
import ycxx.pp;

namespace pipeline = ycxx::pipeline;

int main() {
  pipeline::Session session;
  ycxx::pp::Config config;
  config.target = &ycxx::ast::x86_64_linux;
  const pipeline::TuId unit{0};
  session.set_source(unit, "answer.cpp",
                     "extern \"C\" int answer() { return 42; }",
                     "answer.o", {}, config);

  const auto& front = session.get<pipeline::TuFront>(unit);
  for (const auto& message : front.messages)
    std::cerr << message.text << '\n';
  if (front.has_errors())
    return 1;

  const auto& compiled = session.get<pipeline::Tu>(pipeline::Tu::Key{unit});
  if (!compiled.object) {
    std::cerr << pipeline::Tu::errors(compiled);
    return 1;
  }
  auto bytes = ycxx::obj::elf::write_object(*compiled.object);
  if (!bytes) {
    std::cerr << bytes.error() << '\n';
    return 1;
  }
  if (auto error = pipeline::write_file("answer.o", *bytes, false)) {
    std::cerr << *error << '\n';
    return 1;
  }
}
```

To edit a unit, call `set_source` again with the same `TuId` and ask for a new result.
Keep the session alive while using its query results. Copy any object or diagnostics you want
to retain across updates. To compile a source file that includes headers, call `ycxx::pp::with_system_dirs` or populate
`pp::Config` with the include paths for the target. In an installed or relocated SDK,
`pp::with_system_dirs` and `pp::resource_include_dir` resolve the installed resource and target
libycxx headers automatically. The installed package also exposes `ycxx_RESOURCE_DIR` and
`ycxx_TARGET_INCLUDE_DIR` for explicit configuration through `pp::Config::resource_dir` /
`target_include_dir` or `pp::Config::angle_dirs`. [The compiler example](../examples/embedding/compiler.cpp)
shows how to add the Linux resource include directory explicitly. The C++ runtime used by your host application
and the runtime you link into the generated program are independent choices. See the
[Darwin runtime inputs](BUILDING.md#linking-ycxx-into-a-c-application) when linking a
Darwin program from an installed SDK.

## Read a project target catalog

Use the `project_build` component. Run this example with the path to a workspace that
contains a `build.ycxx` file. It reads target names, kinds, and source locations without
building them. Change `InspectionFormat::json` to `html` to get a standalone catalog
with links to the declarations.

```cpp
import std;
import ycxx.build.inspection;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Pass the directory that contains build.ycxx\n";
    return 1;
  }
  auto catalog = ycxx::build::show_targets(argv[1], ycxx::build::InspectionFormat::json);
  if (!catalog) {
    std::cerr << catalog.error() << '\n';
    return 1;
  }
  std::cout << *catalog;
}
```
