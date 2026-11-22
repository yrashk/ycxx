"""Exercise the opt-in policy through the compiler driver."""

import pathlib
import subprocess
import sys
import tempfile
import textwrap


compiler, headers, include, *target = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)

    def source(name, body, header="permit.hpp"):
        path = root / f"{name}.cpp"
        path.write_text(f'#include "{header}"\nusing namespace ycxx_affine;\n' + textwrap.dedent(body))
        return path

    def run(path, *flags, error=None):
        args = [compiler, "cxx", "--no-daemon", *target, "-I", headers, "-I", include, *flags, str(path)]
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
    run(good, "-fcheck-policies", "-fsyntax-only")
    # Use the native backend for a local-only body on both supported hosts.
    object_source = source("object", """\
        void build(bool early) {
          auto p = issue();
          {
            const Permit& first = p;
            const auto& second = (p);
            inspect(first);
            if (early) return;
            inspect(second);
          }
          redeem(static_cast<Permit&&>(p));
        }
        void borrow_in_loop(bool repeat, bool stop) {
          auto p = issue();
          while (repeat) {
            const auto& view = p;
            inspect(view);
            if (stop) break;
            repeat = false;
            continue;
          }
          redeem(static_cast<Permit&&>(p));
        }
    """)
    obj = root / "permit.o"
    run(object_source, "-fcheck-policies", "-c", "-o", str(obj))
    assert obj.is_file()

    double = source("double", """\
        void twice(bool early) {
          auto p = issue();
          if (early) redeem(static_cast<Permit&&>(p));
          redeem(static_cast<Permit&&>(p));
        }
    """)
    run(double, "-fsyntax-only")
    spent = "affine resource 'p' may already have been consumed"
    stderr = run(double, "-fcheck-policies", "-fsyntax-only", error=spent)
    assert ":6:" in stderr and f"previous consumption at {double}:5:" in stderr, stderr
    rejected = root / "rejected.o"
    run(double, "-fcheck-policies", "-c", "-o", str(rejected), error=spent)
    assert not rejected.exists()

    template = source("template", """\
        template<class T> void twice() {
          T p = issue();
          redeem(static_cast<T&&>(p));
          inspect(p);
        }
        void instantiate() { twice<Permit>(); }
    """)
    run(template, "-fcheck-policies", "-fsyntax-only", error=spent)

    short_circuit = source("short_circuit", """\
        void short_circuit(bool other) {
          auto p = issue();
          other && (redeem(static_cast<Permit&&>(p)), true);
          inspect(p);
        }
    """)
    run(short_circuit, "-fcheck-policies", "-fsyntax-only", error=spent)

    escaped = source("escape", """\
        void evade() { auto p = issue(); Permit& alias = p; }
    """)
    run(escaped, "-fcheck-policies", "-fsyntax-only", error="resource aliases are outside the checked protocol")

    during_borrow = source("during_borrow", """\
        void consume() {
          auto p = issue();
          const Permit& view = p;
          inspect(view);
          redeem(static_cast<Permit&&>(p));
        }
    """)
    borrowed = "affine resource 'p' may still have a live shared borrow 'view'"
    stderr = run(during_borrow, "-fcheck-policies", "-fsyntax-only", error=borrowed)
    assert f"{during_borrow}:7:" in stderr and f"previous borrow at {during_borrow}:5:" in stderr, stderr

    spent_borrow = source("spent_borrow", """\
        void borrow_spent(bool early) {
          auto p = issue();
          if (early) redeem(static_cast<Permit&&>(p));
          const Permit& view = p;
          inspect(view);
        }
    """)
    run(spent_borrow, "-fcheck-policies", "-fsyntax-only", error=spent)

    borrow_move = source("borrow_move", """\
        void move_while_borrowed() {
          auto tx = begin();
          const Transaction& view = tx;
          auto next = transfer(static_cast<Transaction&&>(tx));
          commit(static_cast<Transaction&&>(next));
        }
    """, header="transaction.hpp")
    run(borrow_move, "-fcheck-policies", "-fsyntax-only",
        error="linear resource 'tx' may still have a live shared borrow 'view'")

    field_scope = source("owned_fields", """\
        struct Group { Transaction first; int label; Transaction second; };
        void dispatch(bool transfer_first) {
          Group group{begin(), 19, begin()};
          {
            const auto& view = group.first;
            commit(static_cast<Transaction&&>(group.second));
          }
          if (transfer_first && group.label == 19) {
            auto moved = transfer(static_cast<Transaction&&>(group.first));
            commit(static_cast<Transaction&&>(moved));
          } else {
            rollback(static_cast<Transaction&&>(group.first));
          }
        }
    """, header="transaction.hpp")
    run(field_scope, "-fcheck-policies", "-c", "-o", str(root / "owned_fields.o"))

    for case, body, expected in (
        ("live", "commit(static_cast<Transaction&&>(group.first));", "linear resource 'group.second' may leave"),
        ("borrow", "const auto& view = group.first; commit(static_cast<Transaction&&>(group.first));",
         "linear resource 'group.first' may still have a live shared borrow 'view'"),
        ("escape", "expose(group);", "outside the checked protocol"),
        ("field_address", "const int* pointer = &group.label;", "outside the checked protocol"),
    ):
        field_case = source(f"owned_field_{case}", f"""\
            struct Group {{ Transaction first; Transaction second; int label; }};
            void expose(Group&) noexcept;
            void run() {{
              Group group{{begin(), begin(), 19}};
              {body}
            }}
        """, header="transaction.hpp")
        run(field_case, "-fcheck-policies", "-fsyntax-only", error=expected)

    unwind_fields = source("owned_fields_unwind", """\
        int may_throw();
        struct Group { Transaction first; int label; };
        void run() {
          Group group{begin(), may_throw()};
          commit(static_cast<Transaction&&>(group.first));
        }
    """, header="transaction.hpp")
    run(unwind_fields, "-fcheck-policies", "-fsyntax-only", error="may unwind while linear resource 'group.first' is live")

    nested_parameter = source("nested_owner_parameter", """\
        struct Inner { Transaction owned; };
        struct Outer { Inner inner; };
        void receive(Outer value) {}
    """, header="transaction.hpp")
    run(nested_parameter, "-fsyntax-only")
    run(nested_parameter, "-fcheck-policies", "-fsyntax-only", error="ownership checking requires by-value resource parameters")

    for case, body, diagnostic in (
        ("temporary", "const Permit& view = issue();", "a shared borrow must bind directly to a named owner"),
        ("reborrow", "const Permit& view = p; const Permit& second = view;",
         "a shared borrow must bind directly to a named owner"),
        ("pointer", "const auto& view = p; const void* address = &view;", "outside the checked protocol"),
        ("capture", "const auto& view = p; auto closure = [&view] { inspect(view); };",
         "outside the checked protocol"),
        ("callback", "const auto& view = p; expose(view);", "outside the checked protocol"),
    ):
        borrow_case = source(f"borrow_{case}", f"""\
            void expose(const Permit&) noexcept;
            void outside_protocol() {{
              auto p = issue();
              {body}
            }}
        """)
        run(borrow_case, "-fsyntax-only")
        run(borrow_case, "-fcheck-policies", "-fsyntax-only", error=diagnostic)

    overload = source("overload", """\
        namespace ycxx_affine { void inspect(Permit&) noexcept; }
        void call_overload() { auto p = issue(); inspect(p); }
    """)
    run(overload, "-fcheck-policies", "-fsyntax-only", error="outside the checked protocol")

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
    run(linear, "-fcheck-policies", "-fsyntax-only")
    native_transaction = source("native_transaction", """\
        void finish() {
          auto tx = begin();
          auto other = transfer(static_cast<Transaction&&>(tx));
          commit(static_cast<Transaction&&>(other));
        }
    """, "transaction.hpp")
    linear_obj = root / "transaction.o"
    run(native_transaction, "-fcheck-policies", "-c", "-o", str(linear_obj))
    assert linear_obj.is_file()

    unclosed = "linear resource '{}' may leave its scope without being consumed or transferred"
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
        """, "linear resource 'tx' may already have been consumed"),
        ("unrelated_unwind", """\
            int unrelated();
            void safe(int) noexcept;
            void unrelated_unwind() noexcept {
              auto tx = begin();
              safe(unrelated());
              commit(static_cast<Transaction&&>(tx));
            }
        """, "operation may unwind while linear resource 'tx' is live"),
        ("explicit_throw", """\
            void explicit_throw() {
              auto tx = begin();
              throw 1;
            }
        """, "operation may unwind while linear resource 'tx' is live"),
        ("transferred_from", """\
            void transferred_from() {
              auto tx = begin();
              auto other = transfer(static_cast<Transaction&&>(tx));
              commit(static_cast<Transaction&&>(tx));
              commit(static_cast<Transaction&&>(other));
            }
        """, "linear resource 'tx' may already have transferred its obligation"),
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
        run(case, "-fcheck-policies", "-fsyntax-only", error=diagnostic)
