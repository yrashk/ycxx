#!/usr/bin/env bash
# Throwaway build spike (issue #3): build solver libraries with GCC 16.2, once against
# libstdc++ ("std") and once against libycxx ("ycxx"), and link a tiny smoke test against each.
#
#   build.sh libycxx                  # build + install libycxx (GCC) into $S/libycxx-gcc
#   build.sh <solver> std|ycxx        # solver: cadical highs z3 gecode chuffed bitwuzla
#
# Sources are expected as shallow clones in $S/src-<name>/<name> (see README.md).
# Each run writes $S/logs/<solver>-<variant>.log and prints the wall time.
set -euo pipefail

S=${S:?set S to the scratch directory}
LIBYCXX_SRC=${LIBYCXX_SRC:-/Users/yrashk/Projects/ycxx/deps/libycxx}
GCC=/opt/homebrew/bin/gcc-16
GXX=/opt/homebrew/bin/g++-16
Y=$S/libycxx-gcc
JOBS=${JOBS:-12}
mkdir -p "$S/logs" "$S/smoke"

solver=$1; variant=${2:-}
src=$S/src-$solver/$solver
out=$S/build-$solver-$variant
pfx=$S/inst-$solver-$variant

case $variant in
  std)  CXX=$GXX;            CC=$GCC;          CMAKE_TC=(-DCMAKE_CXX_COMPILER=$GXX -DCMAKE_C_COMPILER=$GCC) ;;
  ycxx) CXX=$Y/bin/ycxx-c++; CC=$Y/bin/ycxx-cc; CMAKE_TC=(-DCMAKE_TOOLCHAIN_FILE=$Y/lib/cmake/libycxx/toolchain.cmake) ;;
  "")   ;;
  *) echo "variant must be std or ycxx" >&2; exit 2 ;;
esac

cmake_build() {  # cmake_build <extra configure args...>
  cmake -S "$src" -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$pfx" \
        "${CMAKE_TC[@]}" "$@"
  cmake --build "$out" -j "$JOBS"
  cmake --install "$out"
}

smoke() {  # smoke <name> <compile/link args...>; source is $S/smoke/<name>.cpp
  "$CXX" -std=c++26 -O1 "$S/smoke/$1.cpp" -o "$out/$1-smoke" "${@:2}"
  "$out/$1-smoke"
}

fzn_smoke() {  # fzn_smoke <fzn solver binary>: x,y in 1..3, x<y, x+y=5  ->  x=2, y=3
  printf '%s\n' 'var 1..3: x :: output_var;' 'var 1..3: y :: output_var;' \
    'constraint int_lt(x, y);' 'constraint int_lin_eq([1, 1], [x, y], 5);' 'solve satisfy;' > "$S/smoke/tiny.fzn"
  "$1" "$S/smoke/tiny.fzn" | tee "$out/fzn-smoke.out"
  grep -q 'x = 2;' "$out/fzn-smoke.out" && grep -q 'y = 3;' "$out/fzn-smoke.out"
}

run() {
case $solver in
libycxx)
  cmake -S "$LIBYCXX_SRC" -B "$S/libycxx-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=$GCC -DCMAKE_CXX_COMPILER=$GXX -DCMAKE_INSTALL_PREFIX="$Y"
  cmake --build "$S/libycxx-build" && cmake --install "$S/libycxx-build" ;;

cadical)  # own configure script + makefile; build dirs must be direct children of the source
  out=$src/build-$variant
  rm -rf "$out"; mkdir -p "$out"
  (cd "$out" && CXX="$CXX" ../configure -O3 && make -j "$JOBS" libcadical.a cadical)
  cat > "$S/smoke/cadical.cpp" <<'EOF'
#include <cadical.hpp>
#include <cstdio>
// (a|b) & (!a|c) & (!b|!c) & (a) : SAT with a=1,c=1,b=0; adding (!c) makes it UNSAT.
int main() {
  CaDiCaL::Solver s;
  s.set("seed", 0);
  s.limit("conflicts", 1000);              // deterministic resource limit
  for (int lit : {1, 2, 0, -1, 3, 0, -2, -3, 0, 1, 0}) s.add(lit);
  int r1 = s.solve();
  bool ok = r1 == 10 && s.val(1) > 0 && s.val(2) < 0 && s.val(3) > 0;
  s.add(-3); s.add(0);
  int r2 = s.solve();
  std::printf("cadical %s: %d %d\n", s.version(), r1, r2);
  return ok && r2 == 20 ? 0 : 1;
}
EOF
  smoke cadical -I"$src/src" "$out/libcadical.a" ;;

highs)
  # GCC 16.2 ICEs (compare_ics, cp/call.cc:12857) on unqualified abs(v[i]) under libycxx (README);
  # qualify the calls in the one affected file. Harmless for the std build, so done for both.
  perl -pi -e 's/(?<![\w:])abs\(/std::abs(/g' "$src/highs/simplex/HEkkDebug.cpp"
  cmake_build -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DZLIB=OFF -DHIPO=OFF
  cat > "$S/smoke/highs.cpp" <<'EOF'
#include <Highs.h>
#include <cstdio>
// max x + y  s.t.  2x + 2y <= 7,  x - y <= 1,  x,y integer >= 0   -> optimum 3
int main() {
  HighsModel m;
  auto& lp = m.lp_;
  lp.num_col_ = 2; lp.num_row_ = 2;
  lp.sense_ = ObjSense::kMaximize;
  lp.col_cost_ = {1, 1}; lp.col_lower_ = {0, 0}; lp.col_upper_ = {10, 10};
  lp.row_lower_ = {-kHighsInf, -kHighsInf}; lp.row_upper_ = {7, 1};
  lp.a_matrix_.format_ = MatrixFormat::kRowwise;
  lp.a_matrix_.start_ = {0, 2, 4}; lp.a_matrix_.index_ = {0, 1, 0, 1}; lp.a_matrix_.value_ = {2, 2, 1, -1};
  lp.integrality_ = {HighsVarType::kInteger, HighsVarType::kInteger};
  Highs h;
  h.setOptionValue("output_flag", false);
  h.setOptionValue("threads", 1);
  h.setOptionValue("random_seed", 0);
  h.setOptionValue("mip_max_nodes", 1000);
  if (h.passModel(m) != HighsStatus::kOk || h.run() != HighsStatus::kOk) return 2;
  double obj = h.getInfo().objective_function_value;
  std::printf("highs %s: %s obj=%g\n", highsVersion(), h.modelStatusToString(h.getModelStatus()).c_str(), obj);
  return h.getModelStatus() == HighsModelStatus::kOptimal && obj > 2.5 && obj < 3.5 ? 0 : 1;
}
EOF
  smoke highs -I"$pfx/include/highs" "$pfx/lib/libhighs.a"
  # A real instance: presolve's aggregator exercises std::set iterators kept across insertions.
  for m in afiro egout 25fv47 bell5; do
    (cd "$out" && bin/highs --threads 1 --random_seed 0 "$src/check/instances/$m.mps") | grep -E '^(Model status|Objective value|  Status|  Primal bound)'
  done ;;

z3)  # static library only, no executable/tests/bindings
  cmake_build -DBUILD_SHARED_LIBS=OFF -DZ3_BUILD_EXECUTABLE=OFF -DZ3_BUILD_TEST_EXECUTABLES=OFF \
              -DZ3_ENABLE_EXAMPLE_TARGETS=OFF -DZ3_BUILD_DOCUMENTATION=OFF -DZ3_BUILD_PYTHON_BINDINGS=OFF
  cat > "$S/smoke/z3.cpp" <<'EOF'
#include <z3++.h>
#include <cstdio>
// Translation-validation shape: prove x*2 == x<<1 for all 32-bit x, and refute x*3 == x<<1.
int main() {
  z3::config cfg;
  z3::context c(cfg);
  z3::expr x = c.bv_const("x", 32);
  z3::solver s(c);
  z3::params p(c);
  p.set("rlimit", 1000000u);               // deterministic resource limit (not wall clock)
  s.set(p);
  s.add((x * 2) != z3::shl(x, 1));
  auto r1 = s.check();                      // unsat: equivalent
  s.reset();
  s.add((x * 3) != z3::shl(x, 1));
  auto r2 = s.check();                      // sat: counterexample
  std::printf("z3 %s: %d %d\n", Z3_get_full_version(), int(r1), int(r2));
  return r1 == z3::unsat && r2 == z3::sat ? 0 : 1;
}
EOF
  smoke z3 -I"$pfx/include" "$pfx/lib/libz3.a" ;;

chuffed)
  # ZLIB is only used by examples, but find_package(ZLIB) adds the macOS SDK's usr/include as a
  # plain -I, ahead of libycxx's -isystem C wrapper headers (see README); keep it out.
  cmake_build -DBUILD_TESTING=OFF -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON
  fzn_smoke "$pfx/bin/fzn-chuffed" ;;

gecode)
  cmake_build -DGECODE_ENABLE_GIST=OFF -DGECODE_ENABLE_QT=OFF -DGECODE_ENABLE_MPFR=OFF -DGECODE_ENABLE_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
  fzn_smoke "$(find "$pfx" "$out" -name fzn-gecode -type f -perm -u+x | head -1)" ;;

bitwuzla)  # meson (run through uvx); GMP and MPFR from Homebrew are mandatory; CaDiCaL and SymFPU are meson wraps
  export PKG_CONFIG_PATH=/opt/homebrew/opt/gmp/lib/pkgconfig:/opt/homebrew/opt/mpfr/lib/pkgconfig
  rm -rf "$out"
  CC="$CC" CXX="$CXX" uvx meson setup "$out" "$src" --buildtype=release -Ddefault_library=static \
      -Dtesting=disabled -Dunit_testing=disabled -Dpython=false -Dmimalloc=disabled --prefix="$pfx"
  uvx meson compile -C "$out" -j "$JOBS"
  uvx meson install -C "$out"
  cat > "$S/smoke/bitwuzla.cpp" <<'EOF'
#include <bitwuzla/cpp/bitwuzla.h>
#include <cstdio>
using namespace bitwuzla;
// x*2 == x<<1 for all 32-bit x (unsat negation), x*3 == x<<1 refuted (sat).
int main() {
  TermManager tm;
  Options opts;
  Sort bv = tm.mk_bv_sort(32);
  Term x = tm.mk_const(bv, "x");
  auto check = [&](uint64_t k) {
    Bitwuzla b(tm, opts);
    b.assert_formula(tm.mk_term(Kind::DISTINCT, {tm.mk_term(Kind::BV_MUL, {x, tm.mk_bv_value_uint64(bv, k)}),
                                                 tm.mk_term(Kind::BV_SHL, {x, tm.mk_bv_one(bv)})}));
    return b.check_sat();
  };
  Result r1 = check(2), r2 = check(3);
  std::printf("bitwuzla %s: %d %d\n", version(), int(r1), int(r2));
  return r1 == Result::UNSAT && r2 == Result::SAT ? 0 : 1;
}
EOF
  export PKG_CONFIG_PATH=$pfx/lib/pkgconfig:$PKG_CONFIG_PATH
  # shellcheck disable=SC2046
  smoke bitwuzla $(pkg-config --cflags --static --libs bitwuzla) ;;

*) echo "unknown solver $solver" >&2; exit 2 ;;
esac
}

log=$S/logs/$solver${variant:+-$variant}.log
start=$(date +%s)
# Not `if run`: errexit is ignored inside an if condition, and every step must count.
set +e; (set -e; run) >"$log" 2>&1; rc=$?; set -e
if [ $rc = 0 ]; then status=PASS; else status=FAIL; fi
echo "$solver ${variant:-} $status $(( $(date +%s) - start ))s  log: $log"
