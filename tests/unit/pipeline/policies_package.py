"""Build an ordinary C++ consumer of an independent YCXX Policies package."""

import pathlib
import resource
import shutil
import signal
import subprocess
import sys
import tempfile


cmake, compiler, source = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    vendor = root / "vendor"
    shutil.copytree(source, vendor)

    def configure(source, build, *options):
        subprocess.run([cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
                        f"-DCMAKE_CXX_COMPILER={compiler}", *options], check=True, timeout=30)

    prefix = root / "install"
    package_build = root / "package-build"
    configure(vendor, package_build, f"-DCMAKE_INSTALL_PREFIX={prefix}")
    subprocess.run([cmake, "--install", str(package_build)], check=True, timeout=30)
    relocated = root / "relocated"
    prefix.rename(relocated)

    consumer = root / "consumer"
    consumer.mkdir()
    (consumer / "main.cpp").write_text("""\
#include <ycxx/ownership.hpp>
#include <ycxx/ownership/runtime.hpp>
#include <ycxx/capabilities.hpp>
#include <ycxx/effects.hpp>
#include <ycxx/effects/runtime.hpp>
#include <type_traits>

void inspect(const ycxx::ownership::linear_state& owner) noexcept { owner.observe(); }
using Rule = ycxx::ownership::verified_observes<decltype(&inspect), &inspect>;
using Check = ycxx::ownership::use_policy<Rule>;

namespace caps = ycxx::capabilities;
inline constexpr caps::rights_mask read_right = 1;
inline constexpr caps::rights_mask write_right = 2;

class Store {
  friend int main(int, char**);
  inline static int cells_[2]{17, 22};

  static caps::token<Store, read_right> reader(caps::scope_id scope) noexcept {
    return caps::token<Store, read_right>(scope);
  }
  static caps::token<Store, read_right | write_right> editor(caps::scope_id scope) noexcept {
    return caps::token<Store, read_right | write_right>(scope);
  }

 public:
  template<caps::rights_mask Granted> requires caps::grants<Granted, read_right>
  static int get(const caps::token<Store, Granted>& authority, caps::scope_id scope) noexcept {
    authority.template require<read_right>(scope);
    return cells_[scope - 1];
  }

  template<caps::rights_mask Granted> requires caps::grants<Granted, write_right>
  static void set(const caps::token<Store, Granted>& authority, caps::scope_id scope, int value) noexcept {
    authority.template require<write_right>(scope);
    cells_[scope - 1] = value;
  }
};

template<class T>
concept CanWrite = requires(const T& authority) { Store::set(authority, 1, 28); };
template<class T>
concept CanRead = requires(const T& authority) { Store::get(authority, 1); };

struct OtherStore;
using Reader = caps::token<Store, read_right>;
using Editor = caps::token<Store, read_right | write_right>;
static_assert(!std::is_default_constructible_v<Reader>);
static_assert(!std::is_constructible_v<Reader, caps::scope_id>);
static_assert(!std::is_copy_constructible_v<Reader>);
static_assert(std::is_nothrow_move_constructible_v<Reader>);
static_assert(CanRead<Reader> && !CanWrite<Reader> && CanWrite<Editor>);
static_assert(!CanRead<caps::token<OtherStore, read_right>>);

struct ReadEffect;
namespace fx = ycxx::effects;
using ReadBudget = fx::budget<ReadEffect, 2>;
using ReadOnce = fx::set<fx::at_most<ReadEffect, 1>>;
static_assert(!std::is_copy_constructible_v<ReadBudget>);
static_assert(!std::is_move_constructible_v<ReadBudget>);

int read(const Reader& authority, ReadBudget& budget) noexcept {
  budget.spend();
  return Store::get(authority, 1);
}
using ReadPolicy = fx::use_policy<Rule, fx::trusted<decltype(&read), &read, ReadOnce>>;

int main(int argc, char**) {
  ycxx::ownership::linear_state owner;
  auto moved = static_cast<ycxx::ownership::linear_state&&>(owner);
  inspect(moved);
  moved.consume();

  auto reader = Store::reader(1);
  if (argc == 2) return Store::get(reader, 2);  // wrong resource scope
  auto next = static_cast<Reader&&>(reader);
  if (argc == 3) return Store::get(reader, 1);  // moved-from token
  if (argc == 4) {
    next.release();
    return Store::get(next, 1);
  }
  ReadBudget budget;
  if (read(next, budget) != 17 || read(next, budget) != 17) return 1;
  if (argc == 5) return read(next, budget);  // exhausted effect budget
  auto editor = Store::editor(2);
  Store::set(editor, 2, 28);
  return Store::get(editor, 2) != 28;
}
""")
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    (consumer / "CMakeLists.txt").write_text("""\
cmake_minimum_required(VERSION 3.20)
project(consumer LANGUAGES CXX)
if(VENDORED)
  add_subdirectory(../vendor policies)
else()
  find_package(YCXXPolicies 0.1 CONFIG REQUIRED)
endif()
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE YCXXPolicies::policies)
""")
    for name, setting in (("installed", f"-DCMAKE_PREFIX_PATH={relocated}"), ("vendored", "-DVENDORED=ON")):
        build = root / name
        configure(consumer, build, setting)
        subprocess.run([cmake, "--build", str(build)], check=True, timeout=30)
        executable = str(build / "consumer")
        subprocess.run([executable], check=True, timeout=10)
        for args in (("wrong-scope",), ("use", "moved"), ("use", "released", "token"),
                     ("read", "with", "used", "budget")):
            result = subprocess.run([executable, *args], timeout=10)
            assert result.returncode in (-signal.SIGABRT, -signal.SIGILL, -signal.SIGTRAP), result
