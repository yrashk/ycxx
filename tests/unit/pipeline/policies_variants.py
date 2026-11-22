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
    Choice selected(own::first_t{}, issue_receipt());
    if (!selected.holds_first()) return 2;
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
    check("chosen", source)
    portable = root / "portable"
    subprocess.run([host, "-std=c++20", "-I", include, root / "chosen.cpp", "-o", portable],
                   check=True, timeout=90)
    for args in ((), ("affine",)):
        subprocess.run([portable, *args], check=True, timeout=10)
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
    rules.write_text(declarations + activation + "\n")
    check("composed_registry", base, other_sources=(rules,))
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
            rules.write_text(declarations)
            removed = resident()
            expected = check("composed_registry", base, "outside the checked protocol", other_sources=(rules,))
            assert removed.returncode == 1 and removed.stderr == expected, (removed.stderr, expected)
            rules.write_text(declarations + activation + "\n")
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

    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    direct = root / "native.cpp"
    direct.write_text(source.split("int handle(", 1)[0] + native_body)
    leak_text = direct.read_text().replace("    auto receipt =", "    if (argc > 2) return 0;\n    auto receipt =")
    leak = root / "native_leak.cpp"
    leak.write_text(leak_text)
    check("native_leak", leak_text, "owner variant 'selected' may leave its scope")
    native = root / "native"
    variants = [ycxx, "cxx", "--no-daemon", "-nostdlib", "-I", include]
    darwin = [*variants, "--target=arm64-apple-darwin"]
    mach = root / "native-macho"
    subprocess.run([*darwin, "-fcheck-policies", direct, "-o", mach], check=True, timeout=90)
    assert mach.read_bytes()[:4] == bytes.fromhex("cffaedfe")
    # PARITY: by design: each platform executes its own format and ISA. The macOS case above
    # cross-links the same source on Linux.
    if sys.platform == "linux" and platform.machine() in ("x86_64", "amd64"):
        target = [*variants, "--target=x86_64-linux-gnu"]
    elif sys.platform == "darwin" and platform.machine() in ("arm64", "aarch64"):
        target = darwin
    else:
        target = None
    if target is not None:
        subprocess.run([*target, "-fcheck-policies", direct, "-o", native], check=True, timeout=90)
        for args in ((), ("affine",)):
            subprocess.run([native, *args], check=True, timeout=10)
        subprocess.run([*target, leak, "-o", native], check=True, timeout=90)
        result = subprocess.run([native, "strict", "leak"], timeout=10)
        assert result.returncode in (-signal.SIGABRT, -signal.SIGILL, -signal.SIGTRAP)
