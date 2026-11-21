"""The same ownership metadata and runtime adapter work in both enforcement modes."""

import json
import os
import pathlib
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

    def syntax(path, error=None):
        args = [compiler, "cxx", "--no-daemon", "-fcheck-affine", "-fsyntax-only", "-I", include, "-I", examples, str(path)]
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
        struct Ticket { int value; };
        Ticket acquire() noexcept;
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
    """), "linear resource 'ticket' may leave its scope")
    syntax(write("inactive.cpp", """\
        #include "resource.hpp"
        void inactive() { checkout::acquire(); }
    """))
    conflict = syntax(write("conflict.cpp", """\
        #include "operations.hpp"
        using CompetingPolicy = ycxx::ownership::use_policy<ycxx::ownership::affine<checkout::Ticket>>;
    """), "conflicting ownership policies")
    assert "conflict.cpp:2:" in conflict and "previous declaration at" in conflict and "resource.hpp:" in conflict, conflict
    syntax(write("bad_signature.cpp", """\
        #include "resource.hpp"
        using Invalid = checkout::own::use_policy<checkout::Resource,
            checkout::own::consumes<decltype(&checkout::acquire), &checkout::acquire>>;
    """), "ownership operation signature does not match")

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
    syntax(large, "affine resource 'last' may already have been consumed")

    # A policy-only edit must invalidate body proofs when its meaning changes and preserve
    # them when it only changes the spelling of the active policy composition.
    policy_source = """\
        #include <ycxx/ownership.hpp>
        namespace own = ycxx::ownership;
        struct Handle { int value; };
        Handle acquire() noexcept;
        using Fragment = own::policy<own::affine<Handle>, own::creates<decltype(&acquire), &acquire>>;
        using Enabled = own::use_policy<Fragment>;
        void run() { auto handle = acquire(); }
    """
    incremental = write("incremental.cpp", policy_source)
    with tempfile.TemporaryDirectory(prefix="ycp-", dir="/tmp") as sockets:
        env = os.environ | {"YCXX_RUNTIME_DIR": sockets, "YCXX_ROOT": str(root), "YCXX_THREADS": "2"}

        def resident(*options):
            return subprocess.run([compiler, "cxx", "--daemon=require", "-fcheck-affine", "-fsyntax-only",
                                   "-I", include, *options, str(incremental)], cwd=root, env=env,
                                  text=True, capture_output=True, timeout=30)

        try:
            first = resident("--trace-engine=json")
            assert first.returncode == 0, first.stderr
            assert any(json.loads(line).get("kind") == "BuildOwnershipFlow" for line in first.stderr.splitlines())
            write("incremental.cpp", policy_source.replace("use_policy<Fragment>", "use_policy<Fragment, Fragment>"))
            repeated = resident("--trace-engine=json")
            assert repeated.returncode == 0, repeated.stderr
            assert not any(record.get("kind") == "BuildOwnershipFlow" and record.get("outcome") in ("executed", "cutoff")
                           for record in map(json.loads, repeated.stderr.splitlines())), repeated.stderr
            write("incremental.cpp", policy_source.replace("own::affine<Handle>", "own::linear<Handle>"))
            changed = resident()
            cold = syntax(incremental, "linear resource 'handle' may leave its scope")
            assert changed.returncode == 1 and changed.stderr == cold, (changed.stderr, cold)
        finally:
            subprocess.run([compiler, "ctl", "shutdown"], cwd=root, env=env, capture_output=True, timeout=10)

    syntax(write("runtime_good.cpp", """\
        #include "runtime_transaction.hpp"
        void finish_work() {
          auto first = example::start();
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
    syntax(runtime, "linear resource 'first' may leave its scope")
    executable = root / "portable"
    subprocess.run([host, "-std=c++20", "-I", examples, str(runtime), "-o", str(executable)], check=True, timeout=30)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    assert subprocess.run([executable], timeout=10).returncode == 0
    for args in (("early",), ("use", "moved"), ("consume", "again", "now")):
        assert subprocess.run([executable, *args], timeout=10).returncode == -signal.SIGABRT
