"""Build an ordinary C++ consumer of an independent YCXX Policies package."""

import pathlib
import shutil
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

using Empty = ycxx::ownership::policy<>;
using Check = ycxx::ownership::use_policy<Empty>;

int main() {
  ycxx::ownership::linear_state owner;
  auto moved = static_cast<ycxx::ownership::linear_state&&>(owner);
  moved.observe();
  moved.consume();
}
""")
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
        subprocess.run([str(build / "consumer")], check=True, timeout=10)
