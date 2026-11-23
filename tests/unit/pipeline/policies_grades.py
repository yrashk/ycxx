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
        using Four = fx::set<fx::at_most<Send, 4>>;
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
        ("counted_loop", "Two", "for (unsigned i = 0; i < 2u; ++i) send(i); return n;", None),
        ("counted_converted_limit", "Two", "for (unsigned i = 0u; i < 2; ++i) send(i); return n;", None),
        ("counted_signed", "Two", "for (int i = 2; i < 4; ++i) send(i); return n;", None),
        ("counted_unsigned_offset", "Two", "for (unsigned i = 2u; i < 4u; i++) send(i); return n;", None),
        ("counted_negative", "Two", "for (int i = -1; i < 1; ++i) send(i); return n;", None),
        ("counted_descending", "Two", "for (int i = 1; i > -1; i--) send(i); return n;", None),
        ("counted_unsigned_descending", "Two", "for (unsigned i = 2u; i > 0u; --i) send(i); return n;", None),
        ("counted_converted_descending", "Two",
         "for (unsigned i = -1; i > 4294967293u; --i) send(n); return n;", None),
        ("counted_offset_overuse", "Two", "for (int i = 7; i < 10; ++i) send(i); return n;",
         "exceeds its usage grade of 2"),
        ("counted_wrong_direction", "Two", "for (unsigned i = 0; i < 2u; --i) send(i); return n;",
         "unbounded usage"),
        ("counted_wrapping_step", "Two", "for (unsigned i = 0; i < 4294967295u; i += 2u) send(n); return n;",
         "unbounded usage"),
        ("counted_unproven_init", "Two", "for (unsigned i = n; i < 2u; ++i) send(i); return n;",
         "unbounded usage"),
        ("counted_nested", "Four", "for (unsigned i = 0u; i < 2u; ++i) "
         "for (unsigned j = 0u; j < 2u; ++j) send(i + j); return n;", None),
        ("counted_zero", "One", "for (unsigned i = 0u; i < 0u; i++) send(n); return send(n);", None),
        ("counted_continue", "Two", "for (unsigned i = 0u; i < 2u; ++i) { send(i); continue; } return n;", None),
        ("counted_return", "Two", "for (unsigned i = 0u; i < 2u; ++i) { if (n) return send(n); send(n); } return n;",
         None),
        ("counted_return_prefix", "Two", "for (unsigned i = 0u; i < 2u; ++i) "
         "{ if (n--) { send(n); return send(n); } send(n); } return n;", "exceeds its usage grade of 2"),
        ("counted_break", "One", "for (unsigned i = 0u; i < 8u; ++i) { send(n); break; } return n;", None),
        ("counted_overuse", "One", "for (unsigned i = 0u; i < 2u; ++i) send(n); return n;",
         "exceeds its usage grade of 1"),
        ("counted_counter_mutation", "Two", "for (unsigned i = 0; i < 2u; ++i) { --i; send(n); } return n;",
         "unbounded usage"),
        ("counted_counter_alias", "Two", "for (unsigned i = 0; i < 2u; ++i) "
         "{ unsigned& alias = i; --alias; send(n); } return n;", "unbounded usage"),
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

    converted_overuse = declaration.replace("BOUND", "Two").replace(
        "BODY", "for (int i = 4294967295u; i < 2; ++i) send(i); return n;")
    message = syntax(write("counted_converted_overuse.cpp", converted_overuse), error="exceeds its usage grade of 2")
    assert "unbounded usage" not in message, message

    inherited_profile = """\
        #include "grades.hpp"
        struct Defaults {
         protected:
          using Limits = Two;
        };
        struct Profile : Defaults { using Effects = Limits; };
        [[=fx::trusted_contract<One>{}]] int send(int) noexcept;
        [[=fx::contract<Profile::Effects>{}]] int run(int n) noexcept { send(n); return send(n); }
    """
    syntax(write("inherited_profile.cpp", inherited_profile))
    syntax(write("inherited_profile_overuse.cpp", inherited_profile.replace("return send(n);", "send(n); return send(n);")),
           error="exceeds its usage grade of 2")
    syntax(write("private_profile.cpp", inherited_profile.replace("protected:", "private:")),
           error="'Limits' is a private member of 'Defaults'")

    syntax(write("unbounded.cpp", declaration.replace("BOUND", "One").replace("trusted_contract<One>", "trusted_contract<Unbounded>")
                 .replace("BODY", "return send(n);")), error="has no finite usage grade for effect 'Send'")
    syntax(write("overflow.cpp", """\
        #include "grades.hpp"
        using Maximum = fx::set<fx::at_most<Send, 18446744073709551615ULL>>;
        [[=fx::trusted_contract<Maximum>{}]] int send(int) noexcept;
        [[=fx::contract<Maximum>{}]] int run(int n) noexcept { send(n); return send(n); }
    """), error="unbounded usage")
    syntax(write("loop_overflow.cpp", (root / "overflow.cpp").read_text().replace(
        "send(n); return send(n);", "for (unsigned i = 0u; i < 2u; ++i) send(n); return n;")),
           error="unbounded usage")
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

            initial_limit = "struct LoopConfig { static constexpr unsigned limit = 2u; };\n"
            header = write("loop_limit.hpp", initial_limit)
            config = write("loop_config.hpp", '#include "loop_limit.hpp"\n')
            counted = write("counted_config.cpp", '#include "loop_config.hpp"\n' +
                (root / "counted_loop.cpp").read_text().replace("i < 2u", "i < LoopConfig::limit"))
            object_path = counted.with_suffix(".o")
            depfile = counted.with_suffix(".d")
            counted_args = [*base, "--daemon=require", "-MD", "-MF", depfile, "-c", counted, "-o", object_path]
            invoke(counted_args, env=env)
            dependencies = depfile.read_text()
            assert all(str(path) in dependencies for path in (counted, config, header)), dependencies
            artifacts = [(path.read_bytes(), path.stat().st_mtime_ns) for path in (object_path, depfile)]
            stamp = header.stat()
            write(header.name, initial_limit.replace("2u", "3u"))
            os.utime(header, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            updated = header.stat()
            assert (updated.st_size, updated.st_mtime_ns) == (stamp.st_size, stamp.st_mtime_ns)
            error = "exceeds its usage grade of 2"
            assert invoke(counted_args, error, env) == syntax(counted, error=error)
            assert [(path.read_bytes(), path.stat().st_mtime_ns) for path in (object_path, depfile)] == artifacts
            write(header.name, initial_limit)
            os.utime(header, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            invoke(counted_args, env=env)
            write(header.name, initial_limit.replace("static constexpr", "inline static"))
            syntax(counted, error="unbounded usage")
        finally:
            subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=env, capture_output=True, timeout=10)

    write("linked_grade.hpp", """\
        #pragma once
        #include "grades.hpp"
        template<int Requested> struct LinkProfile {
          enum Counts : int { previous = Requested - 1, count };
          static constexpr int limit = static_cast<int>(Counts::count);
        };
        using LinkedOne = fx::set<fx::at_most<Send, LinkProfile<1>::limit>>;
    """)
    provider_source = """\
        #include "linked_grade.hpp"
        [[=fx::contract<LinkedOne>{}]] int send(int n) noexcept { return n + LinkProfile<1>::limit; }
    """
    provider = write("provider.cpp", provider_source)
    changed = write("changed.cpp", provider_source.replace("contract<LinkedOne>", "contract<Two>"))
    linked_caller = write("native.cpp", """\
        #include "linked_grade.hpp"
        [[=fx::contract<LinkedOne>{}]] int send(int) noexcept;
        [[=fx::contract<LinkedOne>{}]] int run(int n) noexcept {
          do { n = send(n); } while (false);
          return n;
        }
        int main() { return run(41) != 42; }
    """)
    # PARITY: by design: cross-link Mach-O everywhere and run each binary on its host ISA.
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
                "[[=fx::contract<LinkedOne>{}]]", "[[gnu::weak]] [[=fx::contract<LinkedOne>{}]]"))
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

    if linux_host or mac_host:
        program = root / "budget"
        mediated = write("mediated.cpp", """\
            #include "grades.hpp"
            #include <ycxx/effects/runtime.hpp>
            namespace site {
              template<class T> concept Wide = sizeof(T) > 1;
              template<class Configuration> consteval unsigned limit_for() {
                return static_cast<unsigned>(Configuration::limit);
              }
            }
            struct Deployment {
              constexpr int requested(bool wide) const { return wide ? 2 : 1; }
            };
            consteval Deployment load_deployment() {
              Deployment deployment{};
              return deployment;
            }
            struct RequestedLimit {
              int requested;
              int capped = requested < 4 ? requested : 4;
            };
            constexpr RequestedLimit request_limit(int requested) {
              RequestedLimit limit{requested};
              return limit;
            }
            constexpr int uses = 1;
            template<bool Wide> struct Profile {
              static constexpr int uses = request_limit(load_deployment().requested(Wide)).capped;
            };
            template<class Payload> struct NativeConfig : Profile<site::Wide<Payload>> {};
            template<int Requested> struct CountProfile {
              enum Counts : int { previous = Requested - 1, count };
            };
            struct Active : NativeConfig<int> {
              static constexpr int limit = CountProfile<uses>::count;
            };
            static_assert(Active::limit == 2);
            static_assert(NativeConfig<char>::uses == 1);
            template<class Configuration> struct GradeProfile {
              using Scope = fx::budget<Send, site::limit_for<Configuration>()>;
              using Bound = fx::set<fx::at_most<Send, site::limit_for<Configuration>()>>;
            };
            using Scope = GradeProfile<Active>::Scope;
            using Bound = GradeProfile<Active>::Bound;
            [[=fx::trusted_contract<One>{}]] int transmit(Scope& scope, int value) noexcept {
              scope.spend();
              return value;
            }
            [[=fx::contract<Bound>{}]] int dispatch(Scope& scope) noexcept {
              for (int i = Active::limit; i > 0; --i) transmit(scope, i);
              return 2;
            }
            int main(int argc, char**) {
              Scope scope;
              if (dispatch(scope) != 2) return 1;
              if (argc > 1) transmit(scope, 3);
              return 0;
            }
        """)
        too_many = write("mediated_overuse.cpp", mediated.read_text().replace("i = Active::limit;", "i = 3;"))
        write("split_budget.hpp", """\
            #pragma once
            #include "linked_grade.hpp"
            #include <ycxx/effects/runtime.hpp>
            using SplitScope = fx::budget<Send, LinkProfile<2>::limit>;
            using SplitBound = fx::set<fx::at_most<Send, LinkProfile<2>::limit>>;
            int charge(SplitScope&) noexcept;
        """)
        split_provider = write("split_budget_provider.cpp", """\
            #include "split_budget.hpp"
            [[=fx::trusted_contract<One>{}]] void spend(SplitScope& scope) noexcept { scope.spend(); }
            [[=fx::contract<SplitBound>{}]] int charge(SplitScope& scope) noexcept {
              spend(scope);
              return 7;
            }
        """)
        split_caller = write("split_budget_caller.cpp", """\
            #include "split_budget.hpp"
            int forward(SplitScope& scope) noexcept { return charge(scope); }
            using Rules = fx::use_policy<fx::bound<decltype(&charge), &charge, SplitBound>,
                                         fx::bound<decltype(&forward), &forward, SplitBound>>;
            int main(int argc, char**) {
              SplitScope scope;
              scope.spend();
              if (forward(scope) != 7) return 1;
              if (argc > 1) scope.spend();
              return 0;
            }
        """)
        error = "exceeds its usage grade of 2"
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        trap_signals = (-signal.SIGILL, -signal.SIGTRAP) if mac_host else (-signal.SIGILL,)
        for target, can_run in targets:
            resident = ["--daemon=require" if flag == "--no-daemon" else flag for flag in target]
            with tempfile.TemporaryDirectory(prefix="yg-guard-", dir="/tmp") as sockets:
                env = dict(daemon_env, YCXX_RUNTIME_DIR=sockets)
                try:
                    previous = None
                    for checks in (1, 0, 1):
                        mode = f"-DYCXX_POLICIES_RUNTIME_CHECKS={checks}"
                        link = [mode, "-o", program, mediated]
                        invoke([*resident, *link], env=env)
                        warm = program.read_bytes()
                        program.unlink()
                        invoke([*target, *link], env=env)
                        assert program.read_bytes() == warm, (target, checks)
                        assert warm != previous, (target, checks)
                        previous = warm
                        if can_run:
                            subprocess.run([program], check=True, timeout=10)
                            exhausted = subprocess.run([program, "exhausted"], timeout=10)
                            assert exhausted.returncode in (trap_signals if checks else (0,)), exhausted
                        analysis = [mode, "-fsyntax-only", too_many]
                        assert invoke([*resident, *analysis], error, env) == invoke([*target, *analysis], error, env)
                finally:
                    subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=env, capture_output=True, timeout=10)
            # Both objects directly call the same instantiated inline budget member.
            split = root / "split-budget"
            for checks in (0, 1):
                mode = f"-DYCXX_POLICIES_RUNTIME_CHECKS={checks}"
                objects = []
                for source in (split_caller, split_provider):
                    obj = source.with_suffix(".o")
                    invoke([*target, mode, "-c", source, "-o", obj])
                    objects.append(obj)
                invoke([*target, mode, *objects, "-o", split])
                if can_run:
                    subprocess.run([split], check=True, timeout=10)
                    exhausted = subprocess.run([split, "exhausted"], timeout=10)
                    assert exhausted.returncode in (trap_signals if checks else (0,)), exhausted
        assignment = write("budget_assignment.cpp", """\
            #include "grades.hpp"
            #include <ycxx/effects/runtime.hpp>
            using Scope = fx::budget<Send, 2>;
            void reset(Scope& budget, const Scope& other) { budget = other; }
        """)
        syntax(assignment, error="'operator=' is a deleted function")
