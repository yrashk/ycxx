"""Check active owner alternatives with the portable and checked package."""

import os
import pathlib
import platform
import resource
import signal
import subprocess
import sys
import tempfile


ycxx, host, include = sys.argv[1:]
base = pathlib.Path(__file__).with_suffix(".cpp").read_text()
selection = "using Choice = own::variant<Receipt, Permit>;"
activation = "using VariantPolicy = own::use_policy<own::tracks_variant<Choice>>;"
source = base.replace(selection, f"{selection}\n{activation}")
flags = [ycxx, "cxx", "--no-daemon", "-fcheck-policies", "-fsyntax-only", "-I", include]


native_body = """\
int main(int argc, char**) {
  if (argc != 2) {
    auto issued = issue_receipt();
    Choice initial(own::first_t{}, static_cast<Receipt&&>(issued));
    Choice selected(own::first_t{}, initial.take_first());
    if (initial.holds_first() || !selected.holds_first()) return 2;
    {
      const auto& view = selected.first();
      read(view);
    }
    auto receipt = selected.take_first();
    close(static_cast<Receipt&&>(receipt));
    return selected.holds_first();
  }
  Choice selected(own::second_t{}, issue_permit());
  if (selected.holds_first()) return 3;
  const auto& view = selected.second();
  read_permit(view);
  return 0;
}
"""

def check(name, text, error=None, other_sources=()):
    path = root / f"{name}.cpp"
    path.write_text(text)
    result = subprocess.run([*flags, str(path), *map(str, other_sources)], capture_output=True, text=True, timeout=90)
    if error is None:
        assert result.returncode == 0, result.stderr
    else:
        assert result.returncode == 1 and error in result.stderr, result.stderr
    return result.stderr


with tempfile.TemporaryDirectory(prefix="ycxx-variant-flow-") as tmp:
    root = pathlib.Path(tmp)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    choices = ((), ("affine",))
    trap_signals = (-signal.SIGABRT, -signal.SIGILL, -signal.SIGTRAP)
    check("chosen", source)
    capability = pathlib.Path(__file__).with_name("policies_variant_capability.cpp").read_text()
    check("capability", capability)
    check("capability_grade_exceeded", capability.replace("fx::at_most<Read, 2>", "fx::at_most<Read, 1>"),
          "exceeds its usage grade of 1")

    chosen = root / "chosen.cpp"
    guarded = root / "capability.cpp"
    for program in (chosen, guarded):
        portable = root / f"{program.stem}-portable"
        subprocess.run([host, "-std=c++20", "-I", include, program, "-o", portable],
                       check=True, timeout=90)
        for args in choices:
            subprocess.run([portable, *args], check=True, timeout=10)
    result = subprocess.run([root / "capability-portable", "affine", "wrong-scope"], timeout=10)
    assert result.returncode in trap_signals
    check("direct_operations", source.replace(
        "      const auto& view = choice.first();\n      read(view);", "      read(choice.first());").replace(
        "    auto receipt = choice.take_first();\n    close(static_cast<Receipt&&>(receipt));",
        "    close(choice.take_first());"))
    check("reflected", source.replace(activation, """
struct Selection { using Policy = own::use_policy<own::tracks_variant<Choice>>; };
struct [[=Selection::Policy{}]] Reflected {};
"""))
    rules = root / "variant_rules.cpp"
    declarations = """\
#include <ycxx/ownership.hpp>
#include <ycxx/ownership/variant.hpp>
namespace own = ycxx::ownership;
class Receipt;
class Permit;
using Choice = own::variant<Receipt, Permit>;
"""
    permit_mode = "using PermitMode = own::use_policy<own::affine<Permit>>;\n"
    rules_source = declarations + permit_mode + activation + "\n"
    caller_source = base.replace("own::linear<Receipt>, own::affine<Permit>,", "own::linear<Receipt>,")
    rules.write_text(rules_source)
    check("composed_registry", caller_source, other_sources=(rules,))
    with tempfile.TemporaryDirectory(prefix="ycv-", dir="/tmp") as sockets:
        env = {k: v for k, v in os.environ.items() if k not in ("YCXX_DAEMON", "YCXX_NO_DAEMON")}
        env.update(YCXX_RUNTIME_DIR=sockets, YCXX_ROOT=str(root), YCXX_THREADS="2")
        resident_flags = ["--daemon=require" if flag == "--no-daemon" else flag for flag in flags]
        caller = root / "composed_registry.cpp"

        def resident():
            return subprocess.run([*resident_flags, str(caller), str(rules)], cwd=root, env=env,
                                  capture_output=True, text=True, timeout=90)

        try:
            initial = resident()
            assert initial.returncode == 0, initial.stderr
            rules.write_text(declarations + permit_mode)
            removed = resident()
            expected = check("composed_registry", caller_source, "outside the checked protocol", other_sources=(rules,))
            assert removed.returncode == 1 and removed.stderr == expected, (removed.stderr, expected)
            rules.write_text(rules_source.replace("own::affine<Permit>", "own::linear<Permit>"))
            strict = resident()
            expected = check("composed_registry", caller_source, "owner variant 'choice' may leave its scope",
                             other_sources=(rules,))
            assert strict.returncode == 1 and strict.stderr == expected, (strict.stderr, expected)
            rules.write_text(rules_source)
            restored = resident()
            assert restored.returncode == 0, restored.stderr
        finally:
            subprocess.run([ycxx, "ctl", "shutdown"], cwd=root, env=env, capture_output=True, timeout=10)
    check("missing_registration", base, "outside the checked protocol")
    check("missing_move_authorization", source.replace("own::move_transfers<Receipt>, ", ""),
          "a tracked variant requires move-transfer contracts for both alternatives")
    check("wrong_side", source.replace("choice.first()", "choice.second()").replace(
        "      read(view);", "      read_permit(view);"),
          "owner variant 'choice' may not hold its second alternative")
    check("taken", source.replace("    close(static_cast<Receipt&&>(receipt));",
          "    close(static_cast<Receipt&&>(receipt));\n    auto again = choice.take_first();"),
          "owner variant 'choice' may not hold its first alternative")
    check("borrowed", source.replace("      read(view);\n    }", "      read(view);").replace(
        "    close(static_cast<Receipt&&>(receipt));", "    close(static_cast<Receipt&&>(receipt));\n    }"),
          "owner variant 'choice' may still have a live shared borrow 'view'")
    check("linear_alternative", source.replace("    auto receipt = choice.take_first();\n"
          "    close(static_cast<Receipt&&>(receipt));", "    return 0;"),
          "owner variant 'choice' may leave its scope")
    check("throwing", "void potentially_throwing();\n" + source.replace("      read(view);",
          "      read(view);\n      potentially_throwing();"),
          "operation may unwind while owner variant 'choice' is live")
    check("escaped", source.replace("  if (choice.holds_first())", "  auto* escaped = &choice;\n"
          "  if (choice.holds_first())"),
          "resource aliases are outside the checked protocol")
    check("bad_type", source.replace("own::tracks_variant<Choice>", "own::tracks_variant<Receipt>"),
          "a tracked variant must name an unqualified ycxx::ownership::variant specialization")
    conflicting = check("conflict", source.replace(activation,
        "using Direct = own::use_policy<own::linear<Choice>>;\n" + activation),
        "a tracked variant cannot also have a direct ownership mode")
    assert "previous declaration at" in conflicting, conflicting
    check("aggregate", source.split("int handle(", 1)[0] + """\
struct Stage { Choice choice; };

int main() {
  Stage stages[2]{{Choice(own::first_t{}, issue_receipt())},
                  {Choice(own::second_t{}, issue_permit())}};
  {
    const auto& shared = stages[1].choice.second();
    read_permit(shared);
    auto receipt = stages[0].choice.take_first();
    close(static_cast<Receipt&&>(receipt));
  }
  return stages[0].choice.holds_first();
}
""")

    direct = root / "native.cpp"
    native_text = source.split("int handle(", 1)[0] + native_body
    direct.write_text(native_text)
    check("moved_into_variant", native_text.replace("    Choice selected", "    read(issued);\n    Choice selected"),
          "linear resource 'issued' may already have transferred its obligation")
    leak_text = native_text.replace("    auto receipt =", "    if (argc > 2) return 0;\n    auto receipt =")
    leak = root / "native_leak.cpp"
    leak.write_text(leak_text)
    check("native_leak", leak_text, "owner variant 'selected' may leave its scope")
    variants = [ycxx, "cxx", "--no-daemon", "-I", include]
    darwin = [*variants, "--target=arm64-apple-darwin"]
    programs = (direct, chosen, guarded)
    for program in programs:
        mach = root / f"{program.stem}-macho"
        subprocess.run([*darwin, "-fcheck-policies", program, "-o", mach], check=True, timeout=90)
        assert mach.read_bytes()[:4] == bytes.fromhex("cffaedfe")
    # PARITY: #641 tracks passing the by-value empty selection tags on Linux x86-64.
    # Pin the exact ABI rejection until these checked programs can run natively there.
    if sys.platform == "linux" and platform.machine() in ("x86_64", "amd64"):
        linux = [*variants, "--target=x86_64-linux-gnu", "-nostdlib"]
        for program in programs:
            native = root / f"{program.stem}-elf"
            rejected = subprocess.run([*linux, "-fcheck-policies", program, "-o", native],
                                      capture_output=True, text=True, timeout=90)
            errors = [line for line in rejected.stderr.splitlines() if ": error:" in line]
            assert rejected.returncode == 1 and errors and not native.exists(), rejected.stderr
            assert all("x86-64 System V ABI:" in line and "unsupported aggregate parameter layout" in line
                       for line in errors), rejected.stderr
    # PARITY: by design: run the linked Mach-O executable on its native host and ISA.
    elif sys.platform == "darwin" and platform.machine() in ("arm64", "aarch64"):
        for program in programs:
            native = root / f"{program.stem}-macho"
            for args in choices:
                subprocess.run([native, *args], check=True, timeout=10)
        bad_scope = subprocess.run([root / "capability-macho", "affine", "wrong-scope"], timeout=10)
        assert bad_scope.returncode in trap_signals
        native = root / "unchecked-leak"
        subprocess.run([*darwin, leak, "-o", native], check=True, timeout=90)
        result = subprocess.run([native, "strict", "leak"], timeout=10)
        assert result.returncode in trap_signals
