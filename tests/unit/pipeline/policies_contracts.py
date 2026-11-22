"""The same ownership metadata and runtime adapter work in both enforcement modes."""

import json
import os
import pathlib
import platform
import resource
import signal
import subprocess
import sys
import tempfile
import textwrap


compiler, host, include, examples = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)

    def write(name, source):
        path = root / name
        path.write_text(textwrap.dedent(source))
        return path

    def syntax(*paths, error=None):
        args = [compiler, "cxx", "--no-daemon", "-fcheck-policies", "-fsyntax-only",
                "-I", include, "-I", examples, *map(str, paths)]
        result = subprocess.run(args, text=True, capture_output=True, timeout=90)
        if error is None:
            assert result.returncode == 0, (args, result.stderr)
        else:
            assert result.returncode == 1 and error in result.stderr, (args, result.stderr)
        return result.stderr

    write("resource.hpp", """\
        #pragma once
        #include <ycxx/ownership.hpp>
        namespace checkout {
        namespace own = ycxx::ownership;
        struct Ticket { int value; mutable int flag; };
        Ticket acquire() noexcept;
        void inspect(const Ticket&) noexcept;
        void submit(Ticket&&) noexcept;
        void submit(int) noexcept;
        using Resource = own::policy<own::linear<Ticket>>;
        }
    """)
    write("operations.hpp", """\
        #pragma once
        #include "resource.hpp"
        namespace checkout {
        using Submit = void (*)(Ticket&&) noexcept;
        using Operations = own::policy<
            own::creates<decltype(&acquire), &acquire>,
            own::consumes<Submit, static_cast<Submit>(&submit)>>;
        using ApplicationPolicy = own::policy<Resource, Operations, own::policy<Resource, Operations>>;
        using Activated = own::use_policy<ApplicationPolicy>;
        }
    """)
    syntax(write("valid.cpp", """\
        #include "operations.hpp"
        void valid() {
          auto ticket = checkout::acquire();
          checkout::submit(42);
          checkout::submit(static_cast<checkout::Ticket&&>(ticket));
        }
    """))
    syntax(write("unfinished.cpp", """\
        #include "operations.hpp"
        void unfinished() {
          auto ticket = checkout::acquire();
          checkout::submit(42);
        }
    """), error="linear resource 'ticket' may leave its scope")
    syntax(write("inactive.cpp", """\
        #include "resource.hpp"
        void inactive() { checkout::acquire(); }
    """))
    conflict = syntax(write("conflict.cpp", """\
        #include "operations.hpp"
        using CompetingPolicy = ycxx::ownership::use_policy<ycxx::ownership::affine<checkout::Ticket>>;
    """), error="conflicting ownership policies")
    assert "conflict.cpp:2:" in conflict and "previous declaration at" in conflict and "resource.hpp:" in conflict, conflict
    syntax(write("bad_signature.cpp", """\
        #include "resource.hpp"
        using Invalid = checkout::own::use_policy<checkout::Resource,
            checkout::own::consumes<decltype(&checkout::acquire), &checkout::acquire>>;
    """), error="ownership operation signature does not match")

    mode = write("mode.cpp", """\
        #include "resource.hpp"
        using Mode = checkout::own::use_policy<checkout::Resource>;
    """)
    operations = write("operations.cpp", """\
        #include "resource.hpp"
        namespace checkout {
        [[=own::operation::creates]] Ticket acquire() noexcept;
        using Submit = void (*)(Ticket&&) noexcept;
        using Terminal = own::use_policy<own::consumes<Submit, static_cast<Submit>(&submit)>>;
        [[=own::operation::observes]] void inspect(const Ticket&) noexcept;
        }
    """)
    caller = write("caller.cpp", """\
        #include "resource.hpp"
        void run() {
          auto ticket = checkout::acquire();
          {
            const auto& view = ticket;
            checkout::inspect(view);
          }
          checkout::submit(static_cast<checkout::Ticket&&>(ticket));
        }
    """)
    syntax(operations, error="ownership operation signature does not match")
    syntax(mode, operations, caller)
    syntax(caller, operations, mode)
    omission = write("omission.cpp", """\
        #include "resource.hpp"
        void run() { auto ticket = checkout::acquire(); }
    """)
    syntax(omission)
    diagnostic = syntax(mode, operations, omission, error="linear resource 'ticket' may leave its scope")
    assert "omission.cpp:2:" in diagnostic, diagnostic
    competing_mode = write("competing_mode.cpp", """\
        #include "resource.hpp"
        using CompetingMode = checkout::own::use_policy<checkout::own::affine<checkout::Ticket>>;
    """)
    diagnostic = syntax(mode, competing_mode, error="conflicting ownership policies")
    assert "competing_mode.cpp:2:" in diagnostic and "previous declaration at" in diagnostic and "resource.hpp:" in diagnostic, diagnostic
    competing_operation = write("competing_operation.cpp", """\
        #include "resource.hpp"
        namespace checkout {
        [[=own::operation::transfers]] void submit(Ticket&&) noexcept;
        }
    """)
    diagnostic = syntax(mode, operations, competing_operation, error="conflicting contracts for an ownership operation")
    assert "competing_operation.cpp:3:" in diagnostic and "previous declaration at" in diagnostic and "operations.cpp:5:" in diagnostic, diagnostic
    rejected = subprocess.run([compiler, "cxx", "--no-daemon", "-fcheck-policies", "-c",
                               "-I", include, *map(str, (mode, operations, competing_operation, caller))],
                              cwd=root, text=True, capture_output=True, timeout=30)
    assert rejected.returncode == 1 and "conflicting contracts" in rejected.stderr, rejected.stderr
    assert not list(root.glob("*.o")), list(root.glob("*.o"))

    # A static function in a header has a separate contract in each translation unit.
    write("private_submit.hpp", """\
        #include "resource.hpp"
        static void private_submit(checkout::Ticket&&) noexcept;
    """)
    private_contract = write("private_contract.cpp", """\
        #include "private_submit.hpp"
        using Terminal = checkout::own::use_policy<checkout::own::consumes<decltype(&private_submit), &private_submit>>;
    """)
    private_caller = write("private_caller.cpp", """\
        #include "private_submit.hpp"
        void run() {
          auto ticket = checkout::acquire();
          private_submit(static_cast<checkout::Ticket&&>(ticket));
        }
    """)
    syntax(mode, operations, private_contract, private_caller, error="outside the checked protocol")

    verified = write("verified.cpp", """\
        #include "resource.hpp"
        namespace checkout {
        using Submit = void (*)(Ticket&&) noexcept;
        using Verify = own::use_policy<Resource, own::creates<decltype(&acquire), &acquire>,
            own::verified_observes<decltype(&inspect), &inspect>, own::consumes<Submit, static_cast<Submit>(&submit)>>;
        }
    """)
    observer_source = """\
        #include "resource.hpp"
        namespace checkout {
        int inspected;
        [[=own::operation::verified_observes]] void inspect(const Ticket& ticket) noexcept {
          int snapshot = ticket.value;
          if (snapshot > 0) inspected = snapshot;
        }
        }
    """
    observer = write("observer.cpp", observer_source)
    syntax(verified, caller, observer)
    syntax(verified, caller, error="verified observation requires a unique supported definition")
    syntax(operations, verified, caller, observer)
    syntax(verified, operations, caller, observer)
    syntax(operations, verified, caller, error="verified observation requires a unique supported definition")
    weak_observer = write("weak_observer.cpp", observer_source.replace("]] void inspect", "]] inline void inspect"))
    syntax(verified, caller, weak_observer, error="verified observation requires a unique supported definition")
    labeled_caller = write("labeled_caller.cpp", caller.read_text().replace(
        "void run()", 'namespace checkout { void inspect(const Ticket&) noexcept asm("other_observer"); }\nvoid run()'))
    syntax(verified, labeled_caller, observer, error="verified observation requires a unique supported definition")

    escaping_observer = """\
        #include "resource.hpp"
        namespace checkout {
        const Ticket* saved;
        const int* saved_field;
        void forward(const Ticket&) noexcept;
        [[=own::operation::verified_observes]] void inspect(const Ticket& ticket) noexcept {
          BODY
        }
        }
    """
    for case, body in (
        ("resource", "saved = &ticket;"),
        ("field", "saved_field = &ticket.value;"),
        ("mutate", "ticket.flag = 42;"),
        ("forward", "forward(ticket);"),
        ("capture", "auto saved = [&ticket] { return ticket.value; };"),
    ):
        invalid = write(f"observer_{case}.cpp", escaping_observer.replace("BODY", body))
        diagnostic = syntax(verified, caller, invalid, error="verified observation cannot expose or mutate")
        assert f"{invalid}:7:" in diagnostic, diagnostic

    # More than a thousand distinct resources in separately reusable groups. The last one has a
    # real obligation so the fixture fails if normalization silently drops the tail of the set.
    groups = []
    for group in range(16):
        entries = [f"own::affine<Resource<{i}>>" for i in range(group * 80, (group + 1) * 80)]
        groups.append(f"using Group{group} = own::policy<{', '.join(entries)}>;")
    combined = ", ".join(f"Group{i}" for i in range(len(groups)))
    large = write("large.cpp", f"""\
        #include <ycxx/ownership.hpp>
        namespace large {{
        namespace own = ycxx::ownership;
        template<int Id> struct Resource {{ int value; }};
        {chr(10).join(groups)}
        Resource<1279> acquire() noexcept;
        void use(Resource<1279>&&) noexcept;
        using Operations = own::policy<own::creates<decltype(&acquire), &acquire>, own::consumes<decltype(&use), &use>>;
        using All = own::use_policy<{combined}, Group0, Group15, Operations>;
        void repeated() {{
          auto last = acquire();
          use(static_cast<Resource<1279>&&>(last));
          use(static_cast<Resource<1279>&&>(last));
        }}
        }}
    """)
    syntax(large, error="affine resource 'last' may already have been consumed")

    annotated_source = """\
        #include <ycxx/ownership.hpp>
        namespace own = ycxx::ownership;
        struct [[=own::resource_mode::linear]] Receipt { int value; };
        [[=own::operation::creates]] Receipt open() noexcept;
        [[=own::operation::consumes]] void close(Receipt&&) noexcept;
        void run() {
          auto receipt = open();
          CLOSE_CALL
        }
    """
    close = "close(static_cast<Receipt&&>(receipt));"
    syntax(write("annotated_good.cpp", annotated_source.replace("CLOSE_CALL", close)))
    syntax(write("annotated_missing.cpp", annotated_source.replace("CLOSE_CALL", "")),
           error="linear resource 'receipt' may leave its scope")
    syntax(write("agree.cpp", annotated_source.replace("CLOSE_CALL", close) + """\
        using ReceiptRules = own::policy<own::linear<Receipt>, own::consumes<decltype(&close), &close>>;
        struct ReceiptSelection { using Active = own::use_policy<ReceiptRules>; };
        struct [[=ReceiptSelection::Active{}]] Activate {};
    """))
    disagreement = syntax(write("disagree.cpp", annotated_source.replace("CLOSE_CALL", close) + """\
        using OtherMode = own::use_policy<own::affine<Receipt>>;
    """), error="conflicting ownership policies for 'Receipt'")
    assert "disagree.cpp:10:" in disagreement and "previous declaration at" in disagreement, disagreement
    syntax(write("attribute_activation.cpp", """\
        #include <ycxx/ownership.hpp>
        namespace own = ycxx::ownership;
        struct Receipt { int value; };
        Receipt open() noexcept;
        using Rules = own::policy<own::linear<Receipt>, own::creates<decltype(&open), &open>>;
        struct ReceiptSelection { using Active = own::use_policy<Rules>; };
        struct [[=ReceiptSelection::Active{}]] Activate {};
        void run() { auto receipt = open(); }
    """), error="linear resource 'receipt' may leave its scope")

    # A policy-only edit must invalidate body proofs when its meaning changes and preserve
    # them when it only changes the spelling of the active policy composition.
    write("incremental.hpp", """\
        #pragma once
        #include <ycxx/ownership.hpp>
        namespace own = ycxx::ownership;
        struct Handle { int value; };
        Handle acquire() noexcept;
    """)
    policy_source = """\
        #include "incremental.hpp"
        using Fragment = own::policy<own::affine<Handle>, own::creates<decltype(&acquire), &acquire>>;
        using Enabled = own::use_policy<Fragment>;
    """
    rules = write("incremental_rules.cpp", policy_source)
    incremental = write("incremental.cpp", """\
        #include "incremental.hpp"
        void run() { auto handle = acquire(); }
    """)
    with tempfile.TemporaryDirectory(prefix="ycp-", dir="/tmp") as sockets:
        env = {k: v for k, v in os.environ.items() if k not in ("YCXX_DAEMON", "YCXX_NO_DAEMON")}
        env.update(YCXX_RUNTIME_DIR=sockets, YCXX_ROOT=str(root), YCXX_THREADS="2")

        def resident(*options, sources=(rules, incremental)):
            return subprocess.run([compiler, "cxx", "--daemon=require", "-fcheck-policies", "-fsyntax-only",
                                   "-I", include, *options, *map(str, sources)], cwd=root, env=env,
                                  text=True, capture_output=True, timeout=30)

        try:
            first = resident("--trace-engine=json")
            assert first.returncode == 0, first.stderr
            assert any(json.loads(line).get("kind") == "BuildOwnershipFlow" for line in first.stderr.splitlines())
            write("incremental_rules.cpp", policy_source.replace("use_policy<Fragment>", "use_policy<Fragment, Fragment>"))
            repeated = resident("--trace-engine=json")
            assert repeated.returncode == 0, repeated.stderr
            assert not any(record.get("kind") == "BuildOwnershipFlow" and record.get("outcome") in ("executed", "cutoff")
                           for record in map(json.loads, repeated.stderr.splitlines())), repeated.stderr
            write("incremental_rules.cpp", policy_source.replace("own::affine<Handle>", "own::linear<Handle>"))
            changed = resident()
            cold = syntax(rules, incremental, error="linear resource 'handle' may leave its scope")
            assert changed.returncode == 1 and changed.stderr == cold, (changed.stderr, cold)
            single = resident(sources=(incremental,))
            assert single.returncode == 0, single.stderr
            verified_sources = (verified, caller, observer)
            accepted = resident(sources=verified_sources)
            assert accepted.returncode == 0, accepted.stderr
            write("observer.cpp", escaping_observer.replace("BODY", "saved = &ticket;"))
            denied = resident(sources=verified_sources)
            expected = syntax(*verified_sources, error="verified observation cannot expose or mutate")
            assert denied.returncode == 1 and denied.stderr == expected, (denied.stderr, expected)
            write("observer.cpp", observer_source)
        finally:
            subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=env, capture_output=True, timeout=10)

    syntax(write("runtime_good.cpp", """\
        #include "runtime_transaction.hpp"
        void finish_work() {
          auto first = example::start();
          {
            const auto& view = first;
            example::inspect(view);
          }
          auto second = example::handoff(static_cast<example::Work&&>(first));
          example::finish(static_cast<example::Work&&>(second));
        }
    """))
    runtime = write("portable.cpp", """\
        #include "runtime_transaction.hpp"
        int main(int argc, char**) {
          auto first = example::start();
          if (argc == 2) return 0;
          auto second = example::handoff(static_cast<example::Work&&>(first));
          if (argc == 3) example::finish(static_cast<example::Work&&>(first));
          example::finish(static_cast<example::Work&&>(second));
          if (argc == 4) example::finish(static_cast<example::Work&&>(second));
        }
    """)
    syntax(runtime, error="linear resource 'first' may leave its scope")
    executable = root / "portable"
    subprocess.run([host, "-std=c++20", "-I", include, "-I", examples, str(runtime), "-o", str(executable)], check=True, timeout=30)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    assert subprocess.run([executable], timeout=10).returncode == 0
    for args in (("early",), ("use", "moved"), ("consume", "again", "now")):
        assert subprocess.run([executable, *args], timeout=10).returncode in (
            -signal.SIGABRT, -signal.SIGILL, -signal.SIGTRAP)

    if sys.platform == "linux" and platform.machine() in ("x86_64", "amd64"):
        native = root / "native"
        args = [compiler, "cxx", "--no-daemon", "--target=x86_64-linux-gnu", "-nostdlib", "-I", include, "-I", examples]
        adapter = write("adapter.cpp", """\
            #include "resource.hpp"
            namespace checkout {
            Ticket acquire() noexcept { return {0}; }
            void inspect(const Ticket&) noexcept {}
            void submit(Ticket&&) noexcept {}
            }
        """)
        split_main = write("split_main.cpp", """\
            void run();
            int main() { run(); }
        """)
        subprocess.run([*args, "-fcheck-policies", *map(str, (mode, operations, adapter, caller, split_main)),
                        "-o", str(native)], check=True, timeout=30)
        assert subprocess.run([native], timeout=10).returncode == 0

        providers = write("providers.cpp", """\
            #include "resource.hpp"
            namespace checkout {
            Ticket acquire() noexcept { return {7}; }
            void submit(Ticket&&) noexcept {}
            }
        """)
        observed_main = write("observed_main.cpp", """\
            void run();
            namespace checkout { extern int inspected; }
            int main() {
              run();
              return checkout::inspected != 7;
            }
        """)
        subprocess.run([*args, "-fcheck-policies", *map(str, (verified, caller, observer, providers, observed_main)),
                        "-o", str(native)], check=True, timeout=30)
        assert subprocess.run([native], timeout=10).returncode == 0

        def compile_object(source, checked=True):
            path = source.with_suffix(".o")
            mode_flag = ["-fcheck-policies"] if checked else []
            subprocess.run([*args, *mode_flag, "-c", str(source), "-o", str(path)], check=True, timeout=30)
            return path

        checked_caller = write("checked_caller.cpp", verified.read_text() + caller.read_text())
        checked_observer = write("checked_observer.cpp", mode.read_text() + observer_source)
        caller_object = compile_object(checked_caller)
        observer_object = compile_object(checked_observer)
        other_objects = tuple(map(compile_object, (providers, observed_main)))

        def checked_link(inputs, output, daemon="--no-daemon", env=None):
            command = [option for option in args if option != "--no-daemon"]
            return subprocess.run([*command, daemon, "-fcheck-policies", *map(str, inputs), "-o", str(output)],
                                  cwd=root, env=env, text=True, capture_output=True, timeout=30)

        objects = (caller_object, observer_object, *other_objects)
        linked = checked_link(objects, native)
        assert linked.returncode == 0, linked.stderr
        assert subprocess.run([native], timeout=10).returncode == 0
        mixed = checked_link((checked_caller, observer_object, *other_objects), native)
        assert mixed.returncode == 0, mixed.stderr
        assert subprocess.run([native], timeout=10).returncode == 0
        subprocess.run([*args, *map(str, objects), "-o", str(native)], check=True, timeout=30)
        assert subprocess.run([native], timeout=10).returncode == 0

        unchecked_source = write("unverified_observer.cpp", observer_source.replace(
            "[[=own::operation::verified_observes]] ", ""))
        covered_without_proof = compile_object(unchecked_source)
        unmarked_caller = compile_object(write("unmarked_caller.cpp", checked_caller.read_text()), checked=False)
        stale = write("stale_observer.cpp", mode.read_text().replace("use_policy<checkout::Resource>",
                      "use_policy<checkout::own::affine<checkout::Ticket>>") + observer_source)
        stale_object = compile_object(stale)
        incomplete = root / "incomplete_caller.o"
        contents = caller_object.read_bytes()
        assert b"__ycxx_policy.1.need." in contents
        incomplete.write_bytes(contents.replace(b"__ycxx_policy.1.need.", b"__ycxx_otherx.1.need.", 1))

        for inputs, error in (
            ((caller_object, *other_objects), "requires one strong definition"),
            ((caller_object, covered_without_proof, *other_objects), "no matching guarantee"),
            ((caller_object, stale_object, *other_objects), "no matching guarantee"),
            ((unmarked_caller, observer_object, *other_objects), "missing checked policy coverage"),
            ((incomplete, observer_object, *other_objects), "incomplete checked policy metadata"),
            ((caller_object, observer_object, stale_object, *other_objects), "requires one strong definition"),
        ):
            rejected_output = root / "rejected-link"
            rejected = checked_link(inputs, rejected_output)
            assert rejected.returncode == 1 and error in rejected.stderr, (inputs, rejected.returncode, rejected.stderr)
            assert not rejected_output.exists()

        # Replacing a provider at the same path must not reuse the daemon's previous link verdict.
        snapshot = root / "provider-snapshot.o"
        snapshot.write_bytes(observer_object.read_bytes())
        with tempfile.TemporaryDirectory(prefix="ycp-link-", dir="/tmp") as sockets:
            env = {k: v for k, v in os.environ.items() if k not in ("YCXX_DAEMON", "YCXX_NO_DAEMON")}
            env.update(YCXX_RUNTIME_DIR=sockets, YCXX_ROOT=str(root), YCXX_THREADS="2")
            inputs = (caller_object, snapshot, *other_objects)
            try:
                warm = checked_link(inputs, native, "--daemon=require", env)
                assert warm.returncode == 0, warm.stderr
                snapshot.write_bytes(stale_object.read_bytes())
                stale_link = checked_link(inputs, native, "--daemon=require", env)
                cold = checked_link(inputs, native)
                assert stale_link.returncode == cold.returncode == 1, stale_link.stderr
                assert stale_link.stderr == cold.stderr and "no matching guarantee" in cold.stderr, cold.stderr
            finally:
                subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=env, capture_output=True, timeout=10)

        # Own and discharge the same object through native YCXX, keeping the guard in the ABI.
        native_good = write("native_good.cpp", """\
            #include "runtime_transaction.hpp"
            struct Bundle { example::Work left; example::Work right; };
            int main() {
              Bundle bundle{example::start(), example::start()};
              {
                const auto& view = bundle.left;
                example::inspect(view);
                example::finish(static_cast<example::Work&&>(bundle.right));
              }
              auto second = example::handoff(static_cast<example::Work&&>(bundle.left));
              example::finish(static_cast<example::Work&&>(second));
            }
        """)
        subprocess.run([host, "-std=c++20", "-I", include, "-I", examples, str(native_good), "-o", str(executable)],
                       check=True, timeout=30)
        assert subprocess.run([executable], timeout=10).returncode == 0
        subprocess.run([*args, "-fcheck-policies", str(native_good), "-o", str(native)], check=True, timeout=30)
        assert subprocess.run([native], timeout=10).returncode == 0
        unfinished_field = write("unfinished_field.cpp", native_good.read_text()
                                 .replace("int main()", "int main(int argc, char**)")
                                 .replace("  auto second =", "  if (argc > 1) return 0;\n  auto second ="))
        syntax(unfinished_field, error="linear resource 'bundle.left' may leave its scope")
        subprocess.run([host, "-std=c++20", "-I", include, "-I", examples, str(unfinished_field),
                        "-o", str(executable)], check=True, timeout=30)
        assert subprocess.run([executable, "skip"], timeout=10).returncode == -signal.SIGILL
        subprocess.run([*args, str(unfinished_field), "-o", str(native)], check=True, timeout=30)
        assert subprocess.run([native, "skip"], timeout=10).returncode == -signal.SIGILL

        subprocess.run([*args, str(runtime), "-o", str(native)], check=True, timeout=30)
        assert subprocess.run([native], timeout=10).returncode == 0
        assert subprocess.run([native, "early"], timeout=10).returncode == -signal.SIGILL

        # A hosted peer owns the guard and catches its exception after unwinding through YCXX.
        cleanup = write("cleanup.cpp", """\
            #include <ycxx/ownership/runtime.hpp>
            extern "C" void maybe_throw();
            struct Completion {
              ycxx::ownership::linear_state* state;
              ~Completion() noexcept { state->consume(); }
            };
            extern "C" void run_cleanup(ycxx::ownership::linear_state* state) {
              Completion completion{state};
              maybe_throw();
            }
        """)
        peer = write("cleanup_peer.cpp", """\
            #include <ycxx/ownership/runtime.hpp>
            using ycxx::ownership::linear_state;
            extern "C" void run_cleanup(linear_state*);
            bool throwing;
            extern "C" void maybe_throw() {
              if (throwing) throw 19;
            }
            int main(int argc, char**) {
              {
                linear_state state;
                run_cleanup(&state);
              }
              throwing = true;
              try {
                linear_state state;
                if (argc > 1) state.consume();
                run_cleanup(&state);
              } catch (int value) {
                return value == 19 ? 0 : 1;
              }
              return 2;
            }
        """)
        cleanup_object = root / "cleanup.o"
        subprocess.run([compiler, "cxx", "--no-daemon", "--target=x86_64-linux-gnu", "-I", include, "-c",
                        str(cleanup), "-o", str(cleanup_object)], check=True, timeout=30)
        subprocess.run([host, "-std=c++20", "-I", include, str(peer), str(cleanup_object), "-o", str(native)],
                       check=True, timeout=30)
        assert subprocess.run([native], timeout=10).returncode == 0
        assert subprocess.run([native, "already-consumed"], timeout=10).returncode == -signal.SIGILL

        capability = write("capability.cpp", """\
            #include <ycxx/capabilities.hpp>
            namespace caps = ycxx::capabilities;
            inline constexpr caps::rights_mask read_right = 1;
            inline constexpr caps::rights_mask write_right = 2;
            struct Store;
            using Grant = caps::token<Store, read_right>;
            struct Store {
              static Grant grant() noexcept;
              static int read(const Grant& authority, caps::scope_id scope) noexcept;
            };
            Grant Store::grant() noexcept { return Grant(1); }
            int Store::read(const Grant& authority, caps::scope_id scope) noexcept {
              authority.require<read_right>(scope);
              return scope == 1 ? 17 : 22;
            }
            int main(int argc, char**) {
              auto initial = Store::grant();
              if (argc == 2) return Store::read(initial, 2);
              auto moved = static_cast<Grant&&>(initial);
              if (argc == 3) return Store::read(initial, 1);
              const int value = Store::read(moved, 1);
              moved.release();
              if (argc == 4) return Store::read(moved, 1);
              return value != 17;
            }
        """)
        subprocess.run([*args, "-fcheck-policies", str(capability), "-o", str(native)], check=True, timeout=30)
        assert subprocess.run([native], timeout=10).returncode == 0
        for denied in (("wrong-scope",), ("use", "moved"), ("use", "released", "token")):
            assert subprocess.run([native, *denied], timeout=10).returncode == -signal.SIGILL
        overreach = write("capability_overreach.cpp", capability.read_text().replace(
            "authority.require<read_right>", "authority.require<write_right>"))
        syntax(overreach, error="no matching function for call to 'require'")
        forged = write("capability_forged.cpp", capability.read_text().replace(
            "auto initial = Store::grant();", "Grant initial(1);"))
        syntax(forged, error="private member")
        copied = write("capability_copied.cpp", capability.read_text().replace(
            "static_cast<Grant&&>(initial)", "initial"))
        syntax(copied, error="deleted")

        # The hosted caller receives the nontrivial capability from a YCXX-built issuer.
        declarations, implementation = capability.read_text().split("Grant Store::grant()", 1)
        methods, user_main = implementation.split("int main(", 1)
        capability_provider = write("capability_provider.cpp", declarations + "Grant Store::grant()" + methods)
        capability_peer = write("capability_peer.cpp", declarations + "int main(" + user_main)
        capability_object = compile_object(capability_provider, checked=False)
        subprocess.run([host, "-std=c++20", "-I", include, str(capability_peer), str(capability_object),
                        "-o", str(native)], check=True, timeout=30)
        assert subprocess.run([native], timeout=10).returncode == 0
        assert subprocess.run([native, "wrong-scope"], timeout=10).returncode == -signal.SIGILL

        checked_capability = declarations + textwrap.dedent("""\
            #include <ycxx/ownership.hpp>
            namespace own = ycxx::ownership;
            Grant acquire() noexcept;
            void access(const Grant&) noexcept;
            void release(Grant&&) noexcept;
            using Policy = own::use_policy<own::affine<Grant>,
                own::creates<decltype(&acquire), &acquire>,
                own::observes<decltype(&access), &access>,
                own::consumes<decltype(&release), &release>>;
            void use_grant() {
              auto authority = acquire();
              {
                const auto& view = authority;
                access(view);
              }
              release(static_cast<Grant&&>(authority));
            }
        """)
        syntax(write("capability_ownership.cpp", checked_capability))
        dangling_use = write("capability_after_release.cpp", checked_capability.replace(
            "  release(static_cast<Grant&&>(authority));",
            "  release(static_cast<Grant&&>(authority));\n  access(authority);"))
        syntax(dangling_use, error="affine resource 'authority' may already have been consumed")
