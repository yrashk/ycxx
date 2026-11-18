// Constant evaluation in sema ([expr.const]; query-engine.md §B7; DECISIONS §4.6): an analysed
// expression becomes the L1 body of a thunk, `{ return e; }`, through `function_ir`, and the
// interpreter (ycxx.interp) runs it in constant mode inside the query that analyses the
// expression. Callees' bodies and namespace-scope variables come through `Questions`.
//
// The enclosing function's locals usable in constant expressions (constexpr, or const integral
// with a constant initializer) become const objects of the thunk with their values; naming any
// other local makes the expression one the front end cannot evaluate.
export module ycxx.sema:eval;

import std;
import ycxx.ast;
import ycxx.ir;
import ycxx.irgen;
import ycxx.interp;
import :types;
import :diag;
import :actions;

export namespace ycxx::sema {

// The facts `function_ir` needs (ycxx.irgen:facts), over sema's world and questions.
class EvalFacts : public irgen::TypeFacts {
public:
  EvalFacts(World& w, Questions& q) : TypeFacts(w.ins, sema::target), w_(&w), q_(&q) {}
  ConstId string_address(ConstId s) { return w_->values.address({}, s, 0); }
  bool is_constexpr(EntityId f) { return q_->is_constexpr(f); }
  bool is_main(EntityId) { return false; }  // `main` is never evaluated ([basic.start.main])

private:
  World* w_;
  Questions* q_;
};

// A constant evaluation's outcome: the value, or why there is none (a `{N:r}` argument).
struct Evaluated {
  std::optional<ConstId> value;
  u64 reason = reason_unsupported;
};

class Evaluator {
public:
  Evaluator(Sema& s, Questions& q) : s_(&s), q_(&q) {}

  // Evaluates `e` as a core constant expression of its type, then checks the result is a
  // permitted result of a constant expression ([expr.const]). `e` must be valid.
  Evaluated evaluate(Ex e) {
    if (!e.ok()) return {};
    if (e.glvalue()) e = s_->rvalue(e);
    if (!e.ok()) return {};
    ast::Interners& in = s_->ins();
    ast::Child root = s_->builder().commit(ast::Child{e.id, 0});
    NodeId ret = in.intern(ast::Return{root, in.intern_seq(std::span<const ast::Cleanup>{})}, e.width());
    std::vector<ast::LocalRec> locals(s_->locals().begin(), s_->locals().end());
    for (ast::LocalRec& l : locals) l.decl = NodeId::none;  // drafts of the enclosing frame
    ast::DeclSema thunk{};
    thunk.entity = EntityId::none;
    thunk.type = s_->types().function(Types::unqualified(e.type()), {}, false, LangLinkage::cxx);
    thunk.params = in.intern_seq(std::span<const ast::ParamSema>{});
    irgen::FunctionInput input{EntityId::none, thunk,
                               ast::BodySema{ret, in.intern_seq(std::span<const ast::LocalRec>(locals))},
                               s_->item(), 0, s_->local_values()};
    EvalFacts facts(s_->world(), *q_);
    auto body = irgen::function_ir(in, input, facts);
    if (!body) return {};
    Host host{q_};
    interp::Outcome out = interp::evaluate(host, in, target, body->body, EntityId::none, {}).run();
    if (out.status != interp::Outcome::Status::value) return {std::nullopt, reason_arg(out.failure)};
    if (out.values.size() != 1) return {};
    return constant(out.values[0]);
  }

private:
  struct Host {
    Questions* q;
    interp::ready<interp::Callee> body(EntityId f) { return {q->function_ir(f)}; }
    interp::ready<std::optional<interp::GlobalVar>> global(EntityId v) { return {q->global(v)}; }
    bool stop_requested() const { return false; }  // until the scheduler has stop tokens (#15)
  };

  // The interpreter's result as a constant (§5.5). An address must be of an object with static
  // storage duration or of a function ([expr.const]).
  Evaluated constant(const interp::Result& r) {
    Values& vs = s_->world().values;
    using K = interp::Result::Kind;
    switch (r.kind) {
    case K::integer: return {vs.integer(r.type, r.bits)};
    case K::null: return {vs.null_pointer(r.type)};
    case K::function: return {vs.address(r.type, vs.entity(r.entity), 0)};
    case K::address:
      switch (r.base) {
      case interp::Base::literal: return {vs.address(r.type, r.literal, r.offset)};
      case interp::Base::global: return {vs.address(r.type, vs.entity(r.entity), r.offset)};
      case interp::Base::local:
        return {std::nullopt, reason_arg({interp::Reason::local_address, 0, {}})};
      }
    }
    return {};
  }

  Sema* s_;
  Questions* q_;
};

}  // namespace ycxx::sema
