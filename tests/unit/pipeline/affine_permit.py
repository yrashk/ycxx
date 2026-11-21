"""Exercise the opt-in policy through the compiler driver."""

import pathlib
import subprocess
import sys
import tempfile
import textwrap


compiler, headers, *target = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)

    def source(name, body, header="permit.hpp"):
        path = root / f"{name}.cpp"
        path.write_text(f'#include "{header}"\nusing namespace ycxx_affine;\n' + textwrap.dedent(body))
        return path

    def run(path, *flags, error=None):
        args = [compiler, "cxx", "--no-daemon", *target, "-I", headers, *flags, str(path)]
        result = subprocess.run(args, text=True, capture_output=True, timeout=30)
        if error is None:
            assert result.returncode == 0, (args, result.stderr)
        else:
            assert result.returncode == 1 and error in result.stderr, (args, result.stderr)
        return result.stderr

    good = source("good", """\
        void drop() {
          auto p = issue();
          static_cast<Permit&&>(p);
          inspect(p);
          inspect(p);
        }
        void after_return(bool early, Permit p) {
          if (early) {
            redeem(static_cast<Permit&&>(p));
            return;
          }
          redeem(static_cast<Permit&&>(p));
        }
        void both_arms(bool which) {
          auto p = issue();
          if (which) redeem(static_cast<Permit&&>(p));
          else redeem(static_cast<Permit&&>(p));
        }
        void separate_locals() {
          auto p = issue();
          redeem(static_cast<Permit&&>(p));
          {
            auto p = issue();
            inspect(p);
          }
        }
        void unreachable() {
          auto p = issue();
          if (false) redeem(static_cast<Permit&&>(p));
          true || (redeem(static_cast<Permit&&>(p)), false);
          redeem(static_cast<Permit&&>(p));
        }
    """)
    run(good, "-fcheck-affine", "-fsyntax-only")
    # Use the native backend for a local-only body on both supported hosts.
    object_source = source("object", "void build() { auto p = issue(); inspect(p); }\n")
    obj = root / "permit.o"
    run(object_source, "-fcheck-affine", "-c", "-o", str(obj))
    assert obj.is_file()

    double = source("double", """\
        void twice(bool early) {
          auto p = issue();
          if (early) redeem(static_cast<Permit&&>(p));
          redeem(static_cast<Permit&&>(p));
        }
    """)
    run(double, "-fsyntax-only")
    spent = "affine permit 'p' may already have been consumed"
    stderr = run(double, "-fcheck-affine", "-fsyntax-only", error=spent)
    assert ":6:" in stderr and f"previous consumption at {double}:5:" in stderr, stderr
    rejected = root / "rejected.o"
    run(double, "-fcheck-affine", "-c", "-o", str(rejected), error=spent)
    assert not rejected.exists()

    template = source("template", """\
        template<class T> void twice() {
          T p = issue();
          redeem(static_cast<T&&>(p));
          inspect(p);
        }
        void instantiate() { twice<Permit>(); }
    """)
    run(template, "-fcheck-affine", "-fsyntax-only", error=spent)

    short_circuit = source("short_circuit", """\
        void short_circuit(bool other) {
          auto p = issue();
          other && (redeem(static_cast<Permit&&>(p)), true);
          inspect(p);
        }
    """)
    run(short_circuit, "-fcheck-affine", "-fsyntax-only", error=spent)

    escaped = source("escape", """\
        void evade() { auto p = issue(); const Permit& alias = p; }
    """)
    run(escaped, "-fcheck-affine", "-fsyntax-only", error="affine permit local must be initialized directly")

    overload = source("overload", """\
        namespace ycxx_affine { void inspect(Permit&) noexcept; }
        void call_overload() { auto p = issue(); inspect(p); }
    """)
    run(overload, "-fcheck-affine", "-fsyntax-only", error="outside the checked protocol")

    linear = source("linear", """\
        void safe() noexcept;
        void may_throw();
        void both_arms(bool success, Transaction tx) {
          safe();
          if (success) commit(static_cast<Transaction&&>(tx));
          else rollback(static_cast<Transaction&&>(tx));
          may_throw();
        }
        void before_creation(bool early) {
          if (early) return;
          auto tx = begin();
          auto other = transfer(static_cast<Transaction&&>(tx));
          commit(static_cast<Transaction&&>(other));
        }
        void fresh_per_iteration(int n, bool retry) {
          for (int i = 0; i < n; ++i) {
            auto tx = begin();
            if (retry) {
              rollback(static_cast<Transaction&&>(tx));
              continue;
            }
            commit(static_cast<Transaction&&>(tx));
          }
        }
        void break_exits_initializer() {
          for (auto tx = begin(); true;) {
            commit(static_cast<Transaction&&>(tx));
            break;
          }
        }
        void explicit_short_circuit(bool do_commit) {
          auto tx = begin();
          if (do_commit && (commit(static_cast<Transaction&&>(tx)), true)) return;
          rollback(static_cast<Transaction&&>(tx));
        }
        void do_once() {
          do {
            auto tx = begin();
            commit(static_cast<Transaction&&>(tx));
          } while (false);
        }
    """, "transaction.hpp")
    run(linear, "-fcheck-affine", "-fsyntax-only")
    native_transaction = source("native_transaction", """\
        void finish() {
          auto tx = begin();
          auto other = transfer(static_cast<Transaction&&>(tx));
          commit(static_cast<Transaction&&>(other));
        }
    """, "transaction.hpp")
    linear_obj = root / "transaction.o"
    run(native_transaction, "-fcheck-affine", "-c", "-o", str(linear_obj))
    assert linear_obj.is_file()

    unclosed = "linear transaction '{}' may leave its scope without commit or rollback"
    transaction_errors = (
        ("early_return", """\
            void early_return(bool stop) {
              auto tx = begin();
              if (stop) return;
              commit(static_cast<Transaction&&>(tx));
            }
        """, unclosed.format("tx")),
        ("branch_join", """\
            void branch_join(bool success) {
              auto tx = begin();
              if (success) commit(static_cast<Transaction&&>(tx));
            }
        """, unclosed.format("tx")),
        ("break_early", """\
            void break_early() {
              for (auto tx = begin(); true;) break;
            }
        """, unclosed.format("tx")),
        ("continue_early", """\
            void continue_early(bool again) {
              while (again) {
                auto tx = begin();
                continue;
              }
            }
        """, unclosed.format("tx")),
        ("backedge", """\
            void backedge() {
              auto tx = begin();
              while (true) commit(static_cast<Transaction&&>(tx));
            }
        """, "linear transaction 'tx' may already have been consumed"),
        ("unrelated_unwind", """\
            int unrelated();
            void safe(int) noexcept;
            void unrelated_unwind() noexcept {
              auto tx = begin();
              safe(unrelated());
              commit(static_cast<Transaction&&>(tx));
            }
        """, "operation may unwind while linear transaction 'tx' is live"),
        ("explicit_throw", """\
            void explicit_throw() {
              auto tx = begin();
              throw 1;
            }
        """, "operation may unwind while linear transaction 'tx' is live"),
        ("transferred_from", """\
            void transferred_from() {
              auto tx = begin();
              auto other = transfer(static_cast<Transaction&&>(tx));
              commit(static_cast<Transaction&&>(tx));
              commit(static_cast<Transaction&&>(other));
            }
        """, "linear transaction 'tx' may already have transferred its obligation"),
        ("transferred_to", """\
            void transferred_to() {
              auto tx = begin();
              auto other = transfer(static_cast<Transaction&&>(tx));
            }
        """, unclosed.format("other")),
    )
    for name, body, diagnostic in transaction_errors:
        case = source(name, body, "transaction.hpp")
        run(case, "-fsyntax-only")
        run(case, "-fcheck-affine", "-fsyntax-only", error=diagnostic)
