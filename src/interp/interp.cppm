// ycxx.interp: the L1 interpreter (#25; optimization design §2.3, §10; DECISIONS §4.6). It runs
// constant evaluation and, in `runtime` mode, whole programs.
//
// `evaluate` is the trampolined loop of §2.3 inside a `query_task`: the step machine
// (machine.cppm) runs until it needs a callee's body or a global variable, which the loop fetches
// from the host with `co_await` (lazily, once per evaluation), or until it has run
// `poll_interval` steps, when the loop asks the host whether to stop (`stop_point()` once the
// scheduler, #15, has stop tokens). The step budget is deterministic: a count of nodes.
//
// The host is the query that contains the evaluation (query-engine.md §B7): `decl_sema`,
// `analyze_body` and the like, which answer from `function_ir(F, ctx)` and `constant_value(V,
// ctx)`. Its awaitables complete inline for now, as every engine fetch does until #15.
export module ycxx.interp;

export import :reason;
export import :machine;
import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.ir;

export namespace ycxx::interp {

// `body(e)` and `global(e)` are awaitables: `function_ir` and `constant_value` in the engine.
template <class H>
concept Host = requires(H& h, EntityId e) {
  { h.body(e).await_resume() } -> std::convertible_to<Callee>;
  { h.global(e).await_resume() } -> std::convertible_to<std::optional<GlobalVar>>;
  { h.stop_requested() } -> std::convertible_to<bool>;
};

// An awaitable that is ready with its value, for hosts that answer at once.
template <class T>
struct ready {
  T value;
  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  T await_resume() { return std::move(value); }
};

struct Outcome {
  enum class Status : u8 { value, failed, cancelled };
  Status status = Status::failed;
  std::vector<Result> values;  // the entry's results: none for a void function
  Failure failure;
  u64 steps = 0;
};

// Evaluates `entry` (a function body, or the thunk of an expression) with its parameters' values.
// `name` is the entry's entity in failure sites.
template <Host H>
engine::query_task<Outcome> evaluate(H& host, const ast::Interners& in, const ast::Target& target,
                                     const ir::Function& entry, EntityId name, std::vector<Value> args,
                                     Config config = {}) {
  Machine m(in, target, std::move(config));
  m.start(entry, name, args);
  Outcome out;
  for (;;) {
    Event ev = m.run();
    if (ev == Event::need_body) {
      EntityId e = m.wanted();
      Callee c = co_await host.body(e);
      m.give_body(e, c);
    } else if (ev == Event::need_global) {
      EntityId e = m.wanted();
      std::optional<GlobalVar> g = co_await host.global(e);
      m.give_global(e, std::move(g));
    } else if (ev == Event::poll) {
      if (host.stop_requested()) {
        out.status = Outcome::Status::cancelled;  // nothing half-computed is kept
        break;
      }
    } else {
      out.status = ev == Event::done ? Outcome::Status::value : Outcome::Status::failed;
      if (ev == Event::done) out.values = m.results();
      else out.failure = m.failure();
      break;
    }
  }
  out.steps = m.steps();
  co_return out;
}

// The failure's message, its argument substituted (an op's name for `unsupported`).
std::string message(const Failure& f) {
  std::string_view t = text_of(f.reason);
  std::string arg = f.reason == Reason::unsupported && f.arg < ir::op_count ? std::string(ir::op_table[f.arg].name)
                                                                            : std::format("{}", f.arg);
  std::string out;
  for (std::size_t i = 0; i < t.size(); ++i) {
    if (t.substr(i, 3) == "{0}") {
      out += arg;
      i += 2;
    } else {
      out += t[i];
    }
  }
  return out;
}

}  // namespace ycxx::interp
