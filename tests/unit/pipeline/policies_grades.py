"""Check finite per-effect costs, typed/annotation parity, and checked links."""

import os
import pathlib
import platform
import resource
import signal
import subprocess
import sys
import tempfile
import textwrap


compiler, include = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    base = [compiler, "cxx", "-fcheck-policies", "-I", include]

    def write(name, source):
        path = root / name
        path.write_text(textwrap.dedent(source))
        return path

    def invoke(args, error=None, env=None):
        result = subprocess.run(args, cwd=root, env=env, text=True, capture_output=True, timeout=60)
        if error is None:
            assert result.returncode == 0, (args, result.stderr)
        else:
            assert result.returncode == 1 and error in result.stderr, (args, result.stderr)
        return result.stderr

    def syntax(*sources, error=None):
        return invoke([*base, "--no-daemon", "-fsyntax-only", *sources], error)

    write("grades.hpp", """\
        #pragma once
        #include <ycxx/effects.hpp>
        namespace fx = ycxx::effects;
        struct Send;
        struct SiteConfig { inline static constexpr unsigned slots[2]{1u, 2u}; };
        using One = fx::set<fx::at_most<Send, 1>>;
        using Two = fx::set<fx::at_most<Send, SiteConfig::slots[1]>>;
        using Unbounded = fx::set<Send>;
        int send(int) noexcept;
        int run(int) noexcept;
    """)
    declaration = """\
        #include "grades.hpp"
        [[=fx::trusted_contract<One>{}]] int send(int) noexcept;
        [[=fx::contract<BOUND>{}]] int run(int n) noexcept { BODY }
    """
    for name, bound, body, error in (
        ("twice", "Two", "send(n); return send(n);", None),
        ("too_many", "One", "send(n); return send(n);", "exceeds its usage grade of 1"),
        ("nested_calls", "One", "return send(send(n));", "exceeds its usage grade of 1"),
        ("conditional", "One", "return n ? send(n) : send(0);", None),
        ("if_join", "One", "int r; if (n) r = send(n); else r = send(0); return r;", None),
        ("early_return", "One", "if (n) return send(n); return send(0);", None),
        ("condition_spends", "One", "if (send(n)) return send(0); return 0;", "exceeds its usage grade of 1"),
        ("effect_free_loop", "One", "while (n > 0) --n; return send(n);", None),
        ("loop_initializer", "One", "for (int i = send(n); i > 0; --i) {} return 0;", None),
        ("single_iteration", "One", "do { n = send(n); } while (false); return n;", None),
        ("skipped_loop", "One", "while (false) send(n); return send(0);", None),
        ("loop_condition_spends", "One", "while (send(n)) break; return 0;", None),
        ("break_spends", "One", "while (n-- > 0) { if (!n) { send(n); break; } } return 0;", None),
        ("break_then_spend", "One", "while (n-- > 0) { if (!n) { send(n); break; } } return send(0);",
         "exceeds its usage grade of 1"),
        ("loop_return", "One", "while (n > 0) return send(n); return send(0);", None),
        ("skipped_increment", "One", "for (; n; send(n)) return send(n); return send(0);", None),
        ("effectful_continue", "One", "for (; n; send(n)) continue; return 0;", "unbounded usage"),
        ("nested_break", "One", "while (n-- > 0) { do { send(n); break; } while (n); } return 0;", "unbounded usage"),
        ("effectful_loop", "Two", "while (n-- > 0) send(n); return 0;", "unbounded usage"),
        ("switch", "One", "switch (n) { case 0: return send(n); } return 0;", "without goto or switch"),
        ("recursive", "One", "if (n > 0) return run(n - 1); return send(n);", None),
        ("recursive_spend", "One", "if (n > 0) { send(n); return run(n - 1); } return 0;", "exceeds its usage grade of 1"),
    ):
        syntax(write(f"{name}.cpp", declaration.replace("BOUND", bound).replace("BODY", body)), error=error)

    syntax(write("unbounded.cpp", declaration.replace("BOUND", "One").replace("trusted_contract<One>", "trusted_contract<Unbounded>")
                 .replace("BODY", "return send(n);")), error="has no finite usage grade for effect 'Send'")
    syntax(write("overflow.cpp", """\
        #include "grades.hpp"
        using Maximum = fx::set<fx::at_most<Send, 18446744073709551615ULL>>;
        [[=fx::trusted_contract<Maximum>{}]] int send(int) noexcept;
        [[=fx::contract<Maximum>{}]] int run(int n) noexcept { send(n); return send(n); }
    """), error="unbounded usage")
    syntax(write("bad_set.cpp", declaration.replace("BOUND", "fx::set<Send, fx::at_most<Send, 1>>")
                 .replace("BODY", "return send(n);")), error="conflicting usage grades for the same effect tag")

    rules_source = """\
        #include "grades.hpp"
        using Rules = fx::use_policy<fx::trusted<decltype(&send), &send, One>,
                                    fx::bound<decltype(&run), &run, One>>;
    """
    rules = write("rules.cpp", rules_source)
    caller = write("caller.cpp", """\
        #include "grades.hpp"
        int run(int n) noexcept { return send(n); }
    """)
    annotation = write("parity.cpp", """\
        #include "grades.hpp"
        using Alias = Send;
        using Repeat = fx::set<fx::at_most<Send, 1>, fx::at_most<Alias, 1>>;
        [[=fx::contract<Repeat>{}]] int run(int) noexcept;
    """)
    syntax(annotation, rules, caller)
    conflicting = write("conflict.cpp", annotation.read_text().replace("at_most<Send, 1>", "at_most<Send, 2>")
                        .replace("at_most<Alias, 1>", "at_most<Alias, 2>"))
    message = syntax(rules, conflicting, caller, error="conflicting effect bounds for a function")
    assert f"{conflicting}:4:" in message and f"previous declaration at {rules}:2:" in message, message

    daemon_env = {key: value for key, value in os.environ.items() if key not in ("YCXX_DAEMON", "YCXX_NO_DAEMON")}
    daemon_env.update(YCXX_ROOT=str(root), YCXX_THREADS="2")
    with tempfile.TemporaryDirectory(prefix="yg-", dir="/tmp") as sockets:
        env = dict(daemon_env, YCXX_RUNTIME_DIR=sockets)
        args = [*base, "--daemon=require", "-fsyntax-only", rules, caller]
        try:
            invoke(args, env=env)
            write(rules.name, rules_source.replace("&send, One", "&send, Two"))
            error = "exceeds its usage grade of 1"
            assert invoke(args, error, env) == syntax(rules, caller, error=error)
        finally:
            subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=env, capture_output=True, timeout=10)

    provider_source = """\
        #include "grades.hpp"
        [[=fx::contract<One>{}]] int send(int n) noexcept { return n + 1; }
    """
    provider = write("provider.cpp", provider_source)
    changed = write("changed.cpp", provider_source.replace("contract<One>", "contract<Two>"))
    linked_caller = write("native.cpp", """\
        #include "grades.hpp"
        [[=fx::contract<One>{}]] int send(int) noexcept;
        [[=fx::contract<One>{}]] int run(int n) noexcept {
          do { n = send(n); } while (false);
          return n;
        }
        int main() { return run(41) != 42; }
    """)
    # PARITY: by design: cross-link Mach-O everywhere and run it on arm64 macOS.
    # Execute the ELF contract and budget tests on the Linux x86-64 target.
    darwin = [*base, "--no-daemon", "--target=arm64-apple-darwin"]
    linux = [*base, "--no-daemon", "--target=x86_64-linux-gnu", "-nostdlib"]
    mac_host = sys.platform == "darwin" and platform.machine() == "arm64"
    linux_host = sys.platform.startswith("linux") and platform.machine() == "x86_64"
    targets = [(darwin, mac_host)]
    if linux_host:
        targets.append((linux, True))
    for target, can_run in targets:
        for name, source in (("provider", provider), ("changed", changed), ("caller", linked_caller)):
            invoke([*target, "-c", source, "-o", root / f"{name}.o"])
        program = root / "graded"
        invoke([*target, "-o", program, root / "caller.o", root / "provider.o"])
        if can_run:
            subprocess.run([program], check=True, timeout=10)
        rejected = root / "mismatch"
        invoke([*target, "-o", rejected, root / "caller.o", root / "changed.o"], "no matching guarantee")
        assert not rejected.exists()

        # PARITY: #659 for explicit weak function definitions on Darwin.
        if target == linux:
            weak_provider = write("weak_provider.cpp", provider_source.replace(
                "[[=fx::contract<One>{}]]", "[[gnu::weak]] [[=fx::contract<One>{}]]"))
            weak_object = root / "weak_provider.o"
            invoke([*target, "-c", weak_provider, "-o", weak_object])
            wrong_grade = root / "changed.o"
            for providers in ((weak_object,), (weak_object, wrong_grade), (wrong_grade, weak_object)):
                invoke([*target, "-o", rejected, root / "caller.o", *providers], "requires one strong definition")
                assert not rejected.exists()

        # A resident link must recheck the proof when a provider is rebuilt in place.
        with tempfile.TemporaryDirectory(prefix="yg-link-", dir="/tmp") as sockets:
            link_env = dict(daemon_env, YCXX_RUNTIME_DIR=sockets)
            resident = ["--daemon=require" if flag == "--no-daemon" else flag for flag in target]
            provider_object = root / "provider.o"
            replacement = root / "changed.o"
            link = ["-o", program, root / "caller.o", provider_object]
            try:
                invoke([*resident, *link], env=link_env)
                stamp = provider_object.stat()
                assert provider_object.read_bytes() != replacement.read_bytes()
                os.utime(replacement, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                replacement.replace(provider_object)
                updated = provider_object.stat()
                assert (updated.st_size, updated.st_mtime_ns) == (stamp.st_size, stamp.st_mtime_ns)
                program.unlink()
                error = "no matching guarantee"
                assert invoke([*resident, *link], error, link_env) == invoke([*target, *link], error)
                assert not program.exists()
            finally:
                subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=link_env, capture_output=True, timeout=10)

    if linux_host:
        program = root / "budget"
        mediated = write("mediated.cpp", """\
            #include "grades.hpp"
            #include <ycxx/effects/runtime.hpp>
            using Scope = fx::budget<Send, SiteConfig::slots[1]>;
            [[=fx::trusted_contract<One>{}]] int transmit(Scope& scope, int value) noexcept {
              scope.spend();
              return value;
            }
            [[=fx::contract<Two>{}]] int dispatch(Scope& scope) noexcept {
              do { transmit(scope, 1); } while (false);
              return transmit(scope, 2);
            }
            int main(int argc, char**) {
              Scope scope;
              if (dispatch(scope) != 2) return 1;
              if (argc > 1) transmit(scope, 3);
              return 0;
            }
        """)
        invoke([*linux, "-o", program, mediated])
        subprocess.run([program], check=True, timeout=10)
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        exhausted = subprocess.run([program, "exhausted"], timeout=10)
        assert exhausted.returncode == -signal.SIGILL, exhausted
        assignment = write("budget_assignment.cpp", """\
            #include "grades.hpp"
            #include <ycxx/effects/runtime.hpp>
            using Scope = fx::budget<Send, 2>;
            void reset(Scope& budget, const Scope& other) { budget = other; }
        """)
        syntax(assignment, error="'operator=' is a deleted function")
