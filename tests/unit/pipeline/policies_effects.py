"""Finite effect bounds compose across source inputs and survive policy-only edits."""

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


compiler, include = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)

    def write(name, source):
        path = root / name
        path.write_text(textwrap.dedent(source))
        return path

    base = [compiler, "cxx", "-fcheck-policies", "-I", include]

    def invoke(args, error=None, env=None):
        result = subprocess.run(args, cwd=root, env=env, text=True, capture_output=True, timeout=60)
        if error is None:
            assert result.returncode == 0, (args, result.stderr)
        else:
            assert result.returncode == 1 and error in result.stderr, (args, result.stderr)
        return result.stderr

    def syntax(*sources, error=None):
        return invoke([*base, "--no-daemon", "-fsyntax-only", *sources], error)

    write("effects.hpp", """\
        #pragma once
        #include <ycxx/effects.hpp>
        namespace fx = ycxx::effects;
        namespace file { struct Read; struct Write; }
        namespace net { struct Read; }
        using Read = fx::set<file::Read>;
        using Write = fx::set<file::Write>;
        using Empty = fx::set<>;
        int adapter(int) noexcept;
        int step(int) noexcept;
        int run(int) noexcept;
    """)
    metadata = """\
        #include "effects.hpp"
        using Adapter = fx::policy<fx::trusted<decltype(&adapter), &adapter, Read>>;
        using Pipeline = fx::policy<fx::bound<decltype(&step), &step, Read>,
                                    fx::bound<decltype(&run), &run, Read>>;
        using Active = fx::use_policy<fx::policy<Pipeline, Adapter>, Pipeline>;
    """
    rules = write("rules.cpp", metadata)
    provider_source = """\
        #include "effects.hpp"
        int adapter(int n) noexcept { return n + 4; }
        int step(int n) noexcept { return adapter(n); }
    """
    provider = write("provider.cpp", provider_source)
    caller = write("caller.cpp", """\
        #include "effects.hpp"
        int run(int n) noexcept {
          if (n > 0) return run(n - 1);
          return step(n);
        }
        int main() { return run(2) != 4; }
    """)
    sources = (rules, provider, caller)
    syntax(*sources)
    syntax(caller, provider, rules)
    annotations = write("annotations.cpp", """\
        #include "effects.hpp"
        using Alias = file::Read;
        using TrustedRead = fx::trusted_contract<fx::set<file::Read, Alias>>;
        using CheckedRead = fx::contract<Read>;
        [[=TrustedRead{}]] int adapter(int) noexcept;
        [[=CheckedRead{}]] int step(int) noexcept;
        [[=CheckedRead{}]] int run(int) noexcept;
    """)
    syntax(annotations, provider, caller)
    syntax(*sources, annotations)
    direct_annotations = write("direct_annotations.cpp", """\
        #include "effects.hpp"
        [[=fx::trusted_contract<fx::set<file::Read, file::Read>>{}]] int adapter(int) noexcept;
        [[=fx::contract<Read>{}]] int step(int) noexcept;
        [[=fx::contract<Read>{}]] int run(int) noexcept;
    """)
    syntax(*sources, direct_annotations)
    syntax(rules, caller, error="verified effect bound requires a unique supported definition")
    narrower = write("narrower.cpp", metadata.replace("&run, Read", "&run, Empty"))
    syntax(narrower, provider, caller, error="effect bound of 'step' exceeds the caller's declared bound")
    wrong_domain = write("wrong_domain.cpp", metadata.replace("&run, Read", "&run, fx::set<net::Read>"))
    syntax(wrong_domain, provider, caller, error="effect bound of 'step' exceeds the caller's declared bound")
    conflicting = write("conflicting.cpp", """\
        #include "effects.hpp"
        using CheckedWrite = fx::contract<Write>;
        [[=CheckedWrite{}]] int run(int) noexcept;
    """)
    diagnostic = syntax(*sources, conflicting, error="conflicting effect bounds for a function")
    assert f"{conflicting}:3:" in diagnostic and f"previous declaration at {rules}:3:" in diagnostic, diagnostic
    trust_conflict = write("trust_conflict.cpp", """\
        #include "effects.hpp"
        using CheckedRead = fx::contract<Read>;
        [[=CheckedRead{}]] int adapter(int) noexcept;
    """)
    syntax(*sources, trust_conflict, error="conflicting effect bounds for a function")
    syntax(write("bad_tag.cpp", metadata.replace("&run, Read", "&run, fx::set<int>")), provider, caller,
           error="effect bound requires ycxx::effects::set of unqualified class or enum tags")
    syntax(*sources, write("labeled.cpp", """\
        #include "effects.hpp"
        int adapter(int) noexcept asm("different_adapter");
    """), error="effect bound does not support functions with an assembly label")

    template = """\
        #include "effects.hpp"
        DECLARATIONS
        using CheckedEmpty = fx::contract<Empty>;
        [[=CheckedEmpty{}]] int check() noexcept { BODY }
    """
    for name, declarations, body, error in (
        ("unknown", "int unknown() noexcept;", "return unknown();", "call to 'unknown' has no declared effect bound"),
        ("indirect", "int unknown() noexcept;", "auto f = &unknown; return f();", "indirect call has no verified effect bound"),
        ("constructor", "struct T { T(); };", "T local; return 0;", "local object initialization or cleanup"),
        ("volatile", "volatile int status;", "return status;", "volatile access is outside the checked effect subset"),
        ("unsafe_branch", "int unknown() noexcept;", "if (false) return unknown(); return 0;", "call to 'unknown'"),
        ("discarded_branch", "int unknown() noexcept;", "if constexpr (false) return unknown(); return 0;", None),
        ("assert", "constexpr bool ok() { return true; }", "static_assert(ok()); return 0;", None),
        ("default_arg", "using TrustedRead = fx::trusted_contract<Read>;\n"
                        "using TrustedEmpty = fx::trusted_contract<Empty>;\n"
                        "[[=TrustedRead{}]] int read() noexcept;\n"
                        "[[=TrustedEmpty{}]] int callee(int = read()) noexcept;",
         "return callee();", "effect bound of 'read' exceeds the caller's declared bound"),
    ):
        syntax(write(f"{name}.cpp", template.replace("DECLARATIONS", declarations).replace("BODY", body)), error=error)

    # Exceed a 64-bit rights mask without conflating distinct effect tags.
    tags = ", ".join(f"Tag<{i}>" for i in range(144))
    large = f"""\
        #include "effects.hpp"
        template<int I> struct Tag;
        using All = fx::set<{tags}>;
        using TrustedTail = fx::trusted_contract<fx::set<Tag<143>>>;
        using CheckedAll = fx::contract<All>;
        [[=TrustedTail{{}}]] int tail() noexcept;
        [[=CheckedAll{{}}]] int check() noexcept {{ return tail(); }}
    """
    syntax(write("large.cpp", large))
    syntax(write("large_missing.cpp", large.replace(", Tag<143>>;", ">;")),
           error="effect bound of 'tail' exceeds the caller's declared bound")

    with tempfile.TemporaryDirectory(prefix="yef-", dir="/tmp") as sockets:
        env = {key: value for key, value in os.environ.items() if key not in ("YCXX_DAEMON", "YCXX_NO_DAEMON")}
        env.update(YCXX_RUNTIME_DIR=sockets, YCXX_ROOT=str(root), YCXX_THREADS="2")

        def resident(*options, error=None):
            return invoke([*base, "--daemon=require", "-fsyntax-only", *options, *sources], error, env)

        def ran(trace, query):
            return any(row.get("kind") == query and row.get("outcome") in ("executed", "cutoff")
                       for row in map(json.loads, trace.splitlines()))

        try:
            assert ran(resident("--trace-engine=json"), "CheckEffectBody")
            write(rules.name, metadata.replace("&adapter, Read", "&adapter, fx::set<file::Read, file::Read>"))
            trace = resident("--trace-engine=json")
            assert not ran(trace, "CheckEffectBody") and not ran(trace, "EffectCallsFor"), trace
            write(rules.name, metadata.replace("&adapter, Read", "&adapter, Write"))
            error = "effect bound of 'adapter' exceeds the caller's declared bound"
            assert resident(error=error) == syntax(*sources, error=error)
            write(rules.name, metadata)
            resident()
            write(provider.name, provider_source.replace("return adapter(n);", "return foreign(n);")
                  .replace("int step", "int foreign(int) noexcept;\nint step"))
            error = "call to 'foreign' has no declared effect bound"
            assert resident(error=error) == syntax(*sources, error=error)
            write(provider.name, provider_source)
        finally:
            subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=env, capture_output=True, timeout=10)

    capability_source = """\
        #include <ycxx/capabilities.hpp>
        #include "effects.hpp"
        namespace caps = ycxx::capabilities;
        struct Store;
        using Grant = caps::token<Store, 1>;
        struct Store {
          static Grant grant() noexcept;
        };
        Grant Store::grant() noexcept { return Grant(1); }
        int read_store(const Grant& authority, caps::scope_id scope) noexcept {
          authority.require<1>(scope);
          return 21;
        }
        int checked_read(const Grant& authority, caps::scope_id scope) noexcept {
          return read_store(authority, scope);
        }
        using Rules = fx::use_policy<
            fx::trusted<decltype(&read_store), &read_store, Read>,
            fx::bound<decltype(&checked_read), &checked_read, Read>>;
        int main(int argc, char**) {
          auto authority = Store::grant();
          return checked_read(authority, argc == 1 ? 1 : 2) != 21;
        }
    """
    capability = write("capability.cpp", capability_source)
    syntax(capability)
    denied = write("capability_denied.cpp", capability_source.replace("&checked_read, Read", "&checked_read, Empty"))
    syntax(denied, error="effect bound of 'read_store' exceeds the caller's declared bound")

    handler_source = """\
        #include "effects.hpp"
        #include <ycxx/effects/handler.hpp>
        struct Reader {
          int data;
          int operator()(int index) { return data + index; }
        };
        using Scope = fx::scoped_handler<file::Read, Reader>;
        [[=fx::trusted_contract<Read>{}]] int mediated(Scope& scope, int index) {
          return scope.perform(index);
        }
        [[=fx::contract<Read>{}]] int checked(Scope& scope) { return mediated(scope, 1); }
        int main() {
          Reader first{41}, second{51};
          Scope primary(first);
          {
            Scope alternate(second);
            if (checked(alternate) != 52) return 1;
          }
          return checked(primary) != 42;
        }
    """
    scope_handler = write("handler.cpp", handler_source)
    syntax(scope_handler)
    syntax(write("handler_denied.cpp", handler_source.replace("fx::contract<Read>", "fx::contract<Empty>")),
           error="effect bound of 'mediated' exceeds the caller's declared bound")

    checked_handler_source = """\
        #include "effects.hpp"
        #include <ycxx/effects/handler.hpp>
        using Once = fx::set<fx::at_most<file::Write, 1>>;
        [[=fx::contract<Empty>{}]] int pure(int n) noexcept { return n + 1; }
        [[=fx::trusted_contract<Once>{}]] int write(int n) noexcept { return n + 10; }
        using Scope = fx::scoped_handler<file::Read, decltype(pure)>;
        [[=fx::contract<Once>{}]] int checked(int n) {
          Scope outer(pure);
          int result = outer.perform(n);
          {
            Scope inner(write);
            result += inner.perform(n);
          }
          return result + outer.perform(n);
        }
        int main() { return checked(1) != 15; }
    """
    direct_handler = write("direct_handler.cpp", checked_handler_source)
    syntax(direct_handler)
    typed_handler_source = checked_handler_source.replace("[[=fx::contract<Empty>{}]] ", "")
    typed_handler_source = typed_handler_source.replace("[[=fx::trusted_contract<Once>{}]] ", "")
    typed_handler_source = typed_handler_source.replace("[[=fx::contract<Once>{}]] ", "") + """
        using ActiveHandler = fx::use_policy<
            fx::bound<decltype(&pure), &pure, Empty>,
            fx::trusted<decltype(&write), &write, Once>,
            fx::bound<decltype(&checked), &checked, Once>>;
    """
    syntax(write("typed_handler.cpp", typed_handler_source))
    syntax(write("direct_handler_empty.cpp", checked_handler_source.replace("contract<Once>{}]] int checked",
                 "contract<Empty>{}]] int checked")),
           error="effect bound of 'write' exceeds the caller's declared bound")
    syntax(write("direct_handler_twice.cpp", checked_handler_source.replace("result += inner.perform(n);",
                 "result += inner.perform(n) + inner.perform(n);")), error="exceeds its usage grade of 1")
    syntax(write("direct_handler_escape.cpp", checked_handler_source.replace("int result = outer.perform(n);",
                 "auto& alias = outer; int result = outer.perform(n);")),
           error="a checked scoped handler cannot escape its local perform calls")

    altered_handler = checked_handler_source.replace("#include <ycxx/effects/handler.hpp>", """
        void foreign() noexcept;
        namespace ycxx::effects {
          template<class Tag, class Implementation>
          class scoped_handler {
            Implementation& implementation_;
          public:
            scoped_handler(Implementation& implementation) noexcept : implementation_(implementation) { foreign(); }
            template<class... Args>
            decltype(auto) perform(Args&&... args) { return implementation_(static_cast<Args&&>(args)...); }
          };
        }
    """)
    invalid_wrapper = "scoped handler wrapper must bind and forward to its function without other operations"
    syntax(write("altered_constructor.cpp", altered_handler), error=invalid_wrapper)
    syntax(write("altered_perform.cpp", altered_handler.replace("{ foreign(); }", "{}").replace(
        "{ return implementation_", "{ foreign(); return implementation_")), error=invalid_wrapper)

    wrapper_source = (pathlib.Path(include) / "ycxx" / "effects" / "handler.hpp").read_text()
    wrapper = write("dispatch.hpp", wrapper_source)
    local_handler = write("local_handler.cpp", checked_handler_source.replace(
        "<ycxx/effects/handler.hpp>", '"dispatch.hpp"'))
    with tempfile.TemporaryDirectory(prefix="yef-handler-", dir="/tmp") as sockets:
        handler_env = dict(env, YCXX_RUNTIME_DIR=sockets)
        resident_handler = [*base, "--daemon=require", "-fsyntax-only", local_handler]
        try:
            invoke(resident_handler, env=handler_env)
            write(wrapper.name, wrapper_source.replace("namespace ycxx::effects {",
                "namespace ycxx::effects { void foreign() noexcept;").replace(
                ": implementation_(implementation) {}", ": implementation_(implementation) { foreign(); }"))
            assert invoke(resident_handler, invalid_wrapper, handler_env) == syntax(local_handler, error=invalid_wrapper)
        finally:
            subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=handler_env, capture_output=True, timeout=10)

    checked_provider_declaration = "[[=fx::contract<Empty>{}]] int pure(int n) noexcept"
    checked_provider_definition = checked_provider_declaration + " { return n + 1; }"
    handler_caller = write("handler_caller.cpp", checked_handler_source.replace(
        checked_provider_definition, checked_provider_declaration + ";"))
    handler_peer = write("handler_peer.cpp", '#include "effects.hpp"\n' + checked_provider_definition + "\n")
    unchecked_handler_peer = write("unchecked_handler_peer.cpp", handler_peer.read_text().replace(
        "[[=fx::contract<Empty>{}]] ", ""))

    darwin = [*base, "--no-daemon", "--target=arm64-apple-darwin"]
    darwin_objects = []
    for source in (handler_caller, handler_peer, unchecked_handler_peer):
        obj = source.with_suffix(".darwin.o")
        invoke([*darwin, "-c", source, "-o", obj])
        darwin_objects.append(obj)
    darwin_caller, darwin_peer, darwin_unchecked = darwin_objects
    darwin_program = root / "darwin-handler"
    invoke([*darwin, darwin_caller, darwin_peer, "-o", darwin_program])
    assert darwin_program.read_bytes().startswith(bytes.fromhex("cffaedfe"))
    darwin_rejected = root / "darwin-handler-unverified"
    invoke([*darwin, darwin_caller, darwin_unchecked, "-o", darwin_rejected], error="has no matching guarantee")
    assert not darwin_rejected.exists()

    write("joint.hpp", """\
        #pragma once
        #include <ycxx/effects.hpp>
        namespace own = ycxx::ownership;
        namespace fx = ycxx::effects;
        struct Ticket { int value; };
        void inspect(const Ticket&) noexcept;
        void forward_ticket(const Ticket&) noexcept;
        Ticket issue() noexcept;
        void finish(Ticket&&) noexcept;
        #ifndef EFFECT_ONLY
        using Own = own::use_policy<own::linear<Ticket>,
            own::verified_observes<decltype(&inspect), &inspect>,
            own::verified_observes<decltype(&forward_ticket), &forward_ticket>,
            own::creates<decltype(&issue), &issue>, own::consumes<decltype(&finish), &finish>>;
        #endif
        #ifndef OWNERSHIP_ONLY
        using Bounds = fx::use_policy<
            fx::bound<decltype(&inspect), &inspect, fx::set<>>,
            fx::bound<decltype(&forward_ticket), &forward_ticket, fx::set<>>>;
        #endif
    """)
    joint_leaf = write("joint_leaf.cpp", """\
        #include "joint.hpp"
        void inspect(const Ticket& ticket) noexcept { int value = ticket.value; (void)value; }
    """)
    joint_forward = write("joint_forward.cpp", """\
        #include "joint.hpp"
        void forward_ticket(const Ticket& ticket) noexcept { inspect(ticket); }
    """)
    joint_entry = write("joint_entry.cpp", """\
        #include "joint.hpp"
        Ticket issue() noexcept { return {42}; }
        void finish(Ticket&& ticket) noexcept { if (ticket.value != 42) __builtin_trap(); }
        int main() {
          auto ticket = issue();
          forward_ticket(ticket);
          finish(static_cast<Ticket&&>(ticket));
        }
    """)

    def verify_joint_proofs(target, output):
        objects = [source.with_suffix(".joint.o") for source in (joint_entry, joint_forward, joint_leaf)]
        for source, obj in zip((joint_entry, joint_forward, joint_leaf), objects):
            invoke([*target, "-c", source, "-o", obj])
        invoke([*target, *objects, "-o", output])
        incomplete = root / "joint_incomplete.o"
        denied = root / "joint_denied"
        for retained, missing in (("OWNERSHIP_ONLY", "verified effect bound"),
                                  ("EFFECT_ONLY", "verified observation")):
            invoke([*target, f"-D{retained}", "-c", joint_leaf, "-o", incomplete])
            outcome = invoke([*target, *objects[:2], incomplete, "-o", denied], error=missing)
            assert "has no matching guarantee" in outcome and not denied.exists(), outcome

    joint_darwin = root / "joint-darwin"
    verify_joint_proofs(darwin, joint_darwin)
    assert joint_darwin.read_bytes().startswith(bytes.fromhex("cffaedfe"))

    if sys.platform.startswith("linux") and platform.machine() == "x86_64":
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        executable = root / "effects"
        native = [*base, "--no-daemon", "--target=x86_64-linux-gnu", "-nostdlib"]
        verify_joint_proofs(native, executable)
        subprocess.run([executable], check=True, timeout=10)
        invoke([*native, "-o", executable, *sources, annotations])
        subprocess.run([executable], check=True, timeout=10)
        invoke([*native, "-o", executable, capability])
        subprocess.run([executable], check=True, timeout=10)
        denied_scope = subprocess.run([executable, "wrong-scope"], timeout=10)
        assert denied_scope.returncode == -signal.SIGILL, denied_scope
        invoke([*native, "-o", executable, scope_handler])
        subprocess.run([executable], check=True, timeout=10)
        invoke([*native, "-o", executable, direct_handler])
        subprocess.run([executable], check=True, timeout=10)

        def compile_object(source, name):
            obj = root / (name + ".o")
            invoke([*native, "-c", source, "-o", obj])
            return obj

        def link(*inputs, error=None):
            return invoke([*native, "-o", executable, *inputs], error)

        caller_object = compile_object(handler_caller, "handler_caller")
        peer_object = compile_object(handler_peer, "handler_peer")
        link(caller_object, peer_object)
        subprocess.run([executable], check=True, timeout=10)
        link(caller_object, compile_object(unchecked_handler_peer, "unchecked_handler_peer"),
             error="has no matching guarantee")

        # Independent compilations intern the same set in a different order. The provider
        # forwards through another checked provider, so the first guarantee alone is insufficient.
        leaf_source = """\
            #include "effects.hpp"
            int adapter(int n) noexcept { return n + 4; }
        """
        verified_adapter = "fx::bound<decltype(&adapter), &adapter, Read>"
        leaf = write("leaf.cpp", leaf_source + f"using Leaf = fx::use_policy<{verified_adapter}>;\n")
        stage_source = """\
            #include "effects.hpp"
            int step(int n) noexcept { return adapter(n); }
        """
        stage = write("stage.cpp", stage_source + f"""\
            using Some = fx::set<file::Write, file::Read, file::Read>;
            using Stage = fx::use_policy<{verified_adapter}, fx::bound<decltype(&step), &step, Some>>;
        """)
        entry = write("entry.cpp", caller.read_text() + """\
            using Effects = fx::set<file::Read, file::Write>;
            using Entry = fx::use_policy<fx::bound<decltype(&step), &step, Effects>,
                                         fx::bound<decltype(&run), &run, Effects>>;
        """)
        obj_leaf = compile_object(leaf, "leaf")
        obj_stage = compile_object(stage, "stage")
        obj_entry = compile_object(entry, "entry")
        link(obj_entry, obj_stage, obj_leaf)
        subprocess.run([executable], check=True, timeout=10)
        invoke([arg for arg in native if arg != "-fcheck-policies"] +
               ["-o", executable, obj_entry, obj_stage, obj_leaf])
        subprocess.run([executable], check=True, timeout=10)
        link(entry, obj_stage, obj_leaf)
        subprocess.run([executable], check=True, timeout=10)
        diagnostic = link(obj_entry, obj_stage, error="requires one strong definition")
        assert "verified effect bound" in diagnostic and obj_stage.name in diagnostic, diagnostic

        unchecked_leaf = compile_object(write("unchecked_leaf.cpp", leaf_source), "unchecked_leaf")
        diagnostic = link(obj_entry, obj_stage, unchecked_leaf, error="has no matching guarantee")
        assert "verified effect bound" in diagnostic and unchecked_leaf.name in diagnostic, diagnostic
        different = write("different.cpp", leaf.read_text().replace("&adapter, Read", "&adapter, Empty"))
        different_leaf = compile_object(different, "different")
        diagnostic = link(obj_entry, obj_stage, different_leaf, error="has no matching guarantee")
        assert "verified effect bound" in diagnostic and different_leaf.name in diagnostic, diagnostic

        trusted_stage = write("trusted_stage.cpp", stage.read_text().replace(
            verified_adapter, "fx::trusted<decltype(&adapter), &adapter, Read>"))
        obj_trusted = compile_object(trusted_stage, "trusted_stage")
        link(obj_entry, obj_trusted, unchecked_leaf)
        subprocess.run([executable], check=True, timeout=10)
        diagnostic = link(obj_entry, obj_trusted, obj_leaf, error="conflicts with a verified bound")
        assert obj_trusted.name in diagnostic and obj_leaf.name in diagnostic, diagnostic
        other_trust = write("other_trust.cpp", """\
            #include "effects.hpp"
            int extra_read(int n) noexcept { return adapter(n); }
            using Other = fx::use_policy<fx::trusted<decltype(&adapter), &adapter, Write>,
                                         fx::bound<decltype(&extra_read), &extra_read, Write>>;
        """)
        obj_other_trust = compile_object(other_trust, "other_trust")
        diagnostic = link(obj_entry, obj_trusted, unchecked_leaf, obj_other_trust,
                          error="conflicting trusted effect bound")
        assert obj_trusted.name in diagnostic and obj_other_trust.name in diagnostic, diagnostic

        stripped = root / "stripped.o"
        payload = obj_leaf.read_bytes()
        stripped.write_bytes(payload.replace(b"__ycxx_policy.2.effect.have.", b"__xxxx_policy.2.effect.have."))
        assert stripped.read_bytes() != payload
        link(obj_entry, obj_stage, stripped, error="incomplete checked policy metadata")

        resident_leaf = root / "resident_leaf.o"
        resident_leaf.write_bytes(payload)
        with tempfile.TemporaryDirectory(prefix="yef-link-", dir="/tmp") as sockets:
            link_env = dict(env, YCXX_RUNTIME_DIR=sockets)
            resident_native = [arg if arg != "--no-daemon" else "--daemon=require" for arg in native]
            args = [*resident_native, "-o", executable, obj_entry, obj_stage, resident_leaf]
            try:
                invoke(args, env=link_env)
                replacement = root / "replacement.o"
                replacement.write_bytes(different_leaf.read_bytes())
                replacement.replace(resident_leaf)
                error = "has no matching guarantee"
                assert invoke(args, error, link_env) == link(obj_entry, obj_stage, resident_leaf, error=error)
            finally:
                subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=link_env, capture_output=True, timeout=10)
