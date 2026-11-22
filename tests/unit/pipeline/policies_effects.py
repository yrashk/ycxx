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

    if sys.platform.startswith("linux") and platform.machine() == "x86_64":
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        executable = root / "effects"
        native = [*base, "--no-daemon", "--target=x86_64-linux-gnu", "-nostdlib"]
        invoke([*native, "-o", executable, *sources, annotations])
        subprocess.run([executable], check=True, timeout=10)
        invoke([*native, "-o", executable, capability])
        subprocess.run([executable], check=True, timeout=10)
        denied_scope = subprocess.run([executable, "wrong-scope"], timeout=10)
        assert denied_scope.returncode == -signal.SIGILL, denied_scope

        def compile_object(source, name):
            obj = root / (name + ".o")
            invoke([*native, "-c", source, "-o", obj])
            return obj

        def link(*inputs, error=None):
            return invoke([*native, "-o", executable, *inputs], error)

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
