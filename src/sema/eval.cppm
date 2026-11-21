// Constant evaluation in sema ([expr.const]; query-engine.md §B7; DECISIONS §4.6): an analysed
// expression becomes the L1 body of a thunk, `{ return e; }`, through `function_ir`, and the
// interpreter (ycxx.interp) runs it in constant mode inside the query that analyses the
// expression. Callees' bodies and namespace-scope variables come through `Questions`.
//
// The enclosing function's locals usable in constant expressions (constexpr, or const integral
// with a constant initializer) become const objects of the thunk with their values; any other
// local is an object outside its lifetime, so evaluating a use of it is not constant.
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
  EvalFacts(World& w, Questions& q) : TypeFacts(w.ins, w.target), w_(&w), q_(&q) {
    enum_underlying = [&q](EntityId e) {
      if (const EnumRec* r = enum_record(q, e); r && r->underlying) return *r->underlying;
      return q.classes().underlying(e);
    };
    bases_of = [&q](EntityId c) {
      std::vector<EntityId> r;
      if (const ClassRec* cr = q.classes().find(c))
        for (const BaseRec& b : cr->bases) r.push_back(b.cls);
      return r;
    };
    bit_field_width = [&q](EntityId c, std::uint32_t field) -> std::uint8_t {
      const ClassRec* r = q.classes().find(c);
      return r && field < r->fields.size() ? r->fields[field].bit_width : 0;
    };
    field_type = [&q](EntityId c, std::uint32_t field) -> std::optional<QualType> {
      const ClassRec* r = q.classes().find(c);
      return r && field < r->fields.size() ? std::optional(r->fields[field].type) : std::nullopt;
    };
  }
  ConstId string_address(ConstId s) { return w_->values.address({}, s, 0); }
  bool is_constexpr(EntityId f) { return Sema::closure_member(*w_, f) || q_->is_constexpr(f); }
  bool is_main(EntityId) { return false; }  // `main` is never evaluated ([basic.start.main])
  std::optional<std::uint32_t> ellipsis_after(EntityId f) { return parameters_before_ellipsis(w_->entities.rec(f).type); }

  std::optional<interp::EnumLayout> constant_enum_layout(EntityId e) const {
    const EnumRec* r = enum_record(*q_, e);
    if (!r || !r->underlying) return std::nullopt;
    if (r->fixed) return interp::EnumLayout{*r->underlying, std::nullopt};

    // [dcl.enum]: the hypothetical integer type with the minimum width that holds the enumerators.
    i64 low = 0;
    u64 high = 0;
    for (const EnumeratorRec& enumerator : r->enumerators) {
      if (enumerator.value == ConstId::none) return std::nullopt;
      auto bits = w_->ins.values.get<ast::VInt>(enumerator.value).bits;
      if (ast::is_negative(bits)) low = std::min(low, i64(bits.lo));
      else high = std::max(high, bits.lo);
    }
    const auto magnitude_bits = low < 0 ? std::max(std::bit_width(~u64(low)), std::bit_width(high))
                                        : std::max(1, std::bit_width(high));
    const u64 maximum = magnitude_bits == 64 ? ~u64{0} : (u64{1} << magnitude_bits) - 1;
    if (low < 0 && !std::in_range<i64>(maximum)) return std::nullopt;
    const i64 minimum = low < 0 ? -i64(maximum) - 1 : 0;
    return interp::EnumLayout{*r->underlying, interp::EnumRange{minimum, maximum}};
  }

private:
  static const EnumRec* enum_record(Questions& q, EntityId e) {
    if (const EnumRec* r = q.defining_enum(e)) return r;
    return q.classes().find_enum(e);
  }

  World* w_;
  Questions* q_;
};

// A constant evaluation's outcome: whether it is constant, its value (none for a void
// expression), or why it is not (a `{N:r}` argument).
struct Evaluated {
  std::optional<ConstId> value;
  u64 reason = reason_unsupported;
  bool constant = false;
  // The expression is value-dependent ([temp.dep.constexpr]): it is evaluated when the template is
  // instantiated. Also set when instantiation gave up at its limit, which was diagnosed already.
  bool dependent = false;
};

class Evaluator {
public:
  Evaluator(Sema& s, Questions& q) : s_(&s), q_(&q) {}

  // A direct call to a non-constexpr function cannot initialize a constexpr reference
  // ([expr.const]). This also applies when its class argument cannot yet be evaluated (#25).
  void check_constexpr_call(Ex e) {
    EntityId f = EntityId::none;
    if (auto call = s_->as<ast::Call>(e)) f = call->function;
    if (auto call = s_->as<ast::MemberCall>(e)) f = call->function;
    if (auto call = s_->as<ast::AssignmentCall>(e)) f = call->function;
    if (f != EntityId::none && !Sema::closure_member(s_->world(), f) && !q_->is_constexpr(f))
      s_->diag(e.begin, Diag::not_constant, reason_arg({interp::Reason::not_constexpr, 0, {}}));
  }

  Evaluated evaluate(Ex e) { return evaluate(e, false); }
  Evaluated evaluate_reference_initializer(Ex e) { return evaluate(e, true); }

private:
  // Evaluates `e` as a core constant expression, then checks the result is a permitted result
  // ([expr.const]). The result for a reference initializer is a pointer to the bound object.
  Evaluated evaluate(Ex e, bool reference_initializer) {
    if (!e.ok()) return {};
    if (s_->dependent(e)) return {std::nullopt, reason_unsupported, false, true};
    if (reference_initializer) {
      if (!e.glvalue()) return {};
      // The built-in address of the bound glvalue, without resolving operator& ([dcl.init.ref]).
      ExprInfo info{s_->types().pointer(e.type()), ValueCategory::prvalue, 0};
      e = s_->node(ast::Unary{info, ast::UnaryOp::address_of, Sema::at(e, e.begin)}, e.begin, e.end, info);
    } else if (e.glvalue()) e = s_->rvalue(e);
    if (!e.ok()) return {};
    ast::Interners& in = s_->ins();
    if (s_->types().is_member_pointer(e.type())) {
      // Read a direct member pointer constant from the expression. Its value retains the field
      // identity or typed null for later constant evaluation.
      while (auto p = s_->as<ast::Paren>(e)) e.id = p->operand.id;
      if (auto lit = s_->as<ast::Literal>(e); lit && in.values.is<ast::VMemberPtr>(lit->value))
        return {lit->value, 0, true};
      return {};
    }
    ast::Child root = s_->builder().commit(ast::Child{e.id, 0});
    if (reference_initializer && irgen::materializes_temporary(in.ast, root.id))
      return {std::nullopt, reason_arg({interp::Reason::local_address, 0, {}})};
    NodeId ret = in.intern(ast::Return{root}, e.width());
    std::vector<ast::LocalRec> locals(s_->locals().begin(), s_->locals().end());
    for (ast::LocalRec& l : locals) {
      l.decl = NodeId::none;  // drafts of the enclosing frame
      // In a template's definition a local of dependent type has no object yet; a non-dependent
      // expression cannot name it, so it stands as an `int` outside its lifetime.
      // So does a variable whose initializer is being read to deduce its placeholder type.
      if (s_->types().is_dependent(l.type) || s_->types().has_placeholder(l.type)) l.type = s_->builtin(Builtin::int_);
    }
    ast::DeclSema thunk{};
    thunk.entity = EntityId::none;
    thunk.type = s_->types().function(Types::unqualified(e.type()), {}, false, LangLinkage::cxx);
    thunk.params = in.intern_seq(std::span<const ast::ParamSema>{});
    thunk.template_params = in.intern_seq(std::span<const ast::TemplateParamSema>{});
    irgen::FunctionInput input{EntityId::none, thunk,
                               ast::BodySema{ret, in.intern_seq(std::span<const ast::LocalRec>(locals)),
                                             in.intern_seq(std::span<const ast::ExpansionRec>{}), in.intern_seq(std::span<const u32>{}),
                                             ast::QualType{}, 0, in.intern_seq(std::span<const ast::ClosureDef>{})},
                               s_->item(), 0, true, s_->local_values()};
    EvalFacts facts(s_->world(), *q_);
    auto body = irgen::function_ir(in, input, facts);
    if (!body) return {};
    Host host{q_, this};
    interp::Config config{.enum_layout = [&facts](EntityId e) { return facts.constant_enum_layout(e); }};
    interp::Outcome out = interp::evaluate(host, in, s_->world().target, body->body, EntityId::none, {}, std::move(config)).run();
    if (out.status != interp::Outcome::Status::value && q_->instantiation_gave_up())
      return {std::nullopt, reason_unsupported, false, true};
    if (out.status != interp::Outcome::Status::value) {
      // In the thunk's own frame, the only objects outside their lifetime are the enclosing
      // function's locals that are not usable in constant expressions.
      interp::Failure f = out.failure;
      // A missing interpreter operation may hide a binding that requires static initialization.
      if (reference_initializer && (f.reason == interp::Reason::unsupported || f.reason == interp::Reason::unsupported_constant ||
                                    f.reason == interp::Reason::callee_unsupported))
        return {};
      if (f.reason == interp::Reason::outside_lifetime && !f.stack.empty() && f.stack[0].function == EntityId::none)
        f.reason = interp::Reason::not_usable;
      return {std::nullopt, reason_arg(f)};
    }
    if (out.values.empty() && s_->types().is_void(e.type())) return {std::nullopt, 0, true};
    if (out.values.size() != 1) return {};
    const interp::Result& value = out.values[0];
    if (reference_initializer && value.kind == interp::Result::Kind::null)
      return {std::nullopt, reason_arg({interp::Reason::null_access, 0, {}})};
    if (reference_initializer && value.past)
      return {std::nullopt, reason_arg({interp::Reason::past_end_access, 0, {}})};
    return constant(value);
  }

public:
  // An aggregate initializer is constant when each of its elements is constant ([expr.const]).
  Evaluated evaluate_initializer(Ex e) {
    if (s_->as<ast::InitList>(e)) {
      std::vector<ConstId> elements;
      for (const Ex& element : s_->list_elements(e)) {
        Evaluated result = evaluate_initializer(element);
        if (!result.value) return result;
        elements.push_back(*result.value);
      }
      return {s_->world().values.aggregate(e.type(), elements), 0, true};
    }
    if (s_->as<ast::ValueInit>(e) || s_->as<ast::DefaultInit>(e)) {
      if (auto value = zero_constant(e.type())) return {*value, 0, true};
      return {};
    }
    if (s_->types().is_class(e.type())) return {};  // the interpreter cannot return class objects yet (#25)
    return evaluate(e);
  }

private:
  // The value of a zero-initialized object of type `t` ([dcl.init.general]).
  std::optional<ConstId> zero_constant(QualType t) {
    Types& types = s_->types();
    Values& values = s_->world().values;
    if (s_->is_aggregate(t)) {
      std::vector<ConstId> elements;
      for (const Sema::Element& element : s_->elements(t)) {
        auto value = zero_constant(element.type);
        if (!value) return std::nullopt;
        elements.push_back(*value);
      }
      return values.aggregate(t, elements);
    }
    if (types.is_pointer(t) || types.is(t, Builtin::nullptr_t)) return values.null_pointer(t);
    if (types.is_member_pointer(t) && !types.is_function(types.rec(t).inner))
      return s_->ins().intern_value(ast::VMemberPtr{Types::unqualified(types.canonical(t))});
    if (types.is_enum(t)) return values.integer(s_->underlying_of(t), 0);
    if (types.is_integral(t)) return values.integer(Types::unqualified(t), 0);
    if (types.is_standard_floating(t) && ast::computes(ast::float_format(s_->ins(), s_->world().target, t)))
      return values.floating(Types::unqualified(t), 0.0);
    return std::nullopt;
  }

  struct Host {
    Questions* q;
    Evaluator* e;
    interp::ready<interp::Callee> body(EntityId f) {
      if (auto c = e->closure_callee(f)) return {*c};
      return {q->function_ir(f)};
    }
    interp::ready<std::optional<interp::GlobalVar>> global(EntityId v) { return {q->global(v)}; }
    bool stop_requested() const { return false; }  // until the scheduler has stop tokens (#15)
  };

  // The interpreter's result as a constant (§5.5). An address must be of an object with static
  // storage duration or of a function ([expr.const]).
  Evaluated constant(const interp::Result& r) {
    Values& vs = s_->world().values;
    using K = interp::Result::Kind;
    switch (r.kind) {
    case K::integer: {
      QualType t = s_->types().is_enum(r.type) ? s_->underlying_of(r.type) : r.type;
      return {vs.integer(t, r.bits), 0, true};
    }
    case K::floating: return {ast::float_value(s_->ins(), s_->world().target, r.type, ast::Bits128{r.bits, 0}), 0, true};
    case K::null: return {vs.null_pointer(r.type), 0, true};
    case K::function: return {vs.address(r.type, vs.entity(r.entity), 0), 0, true};
    case K::address: {
      if (r.base == interp::Base::local)
        return {std::nullopt, reason_arg({interp::Reason::local_address, 0, {}})};
      ConstId base = r.base == interp::Base::literal ? r.literal : vs.entity(r.entity);
      std::vector<u32> path;
      // Named arrays and erased pointers retain their domain even at a shared byte offset
      // ([basic.compound], [conv.ptr]).
      bool erased = s_->types().is_pointer(r.type) && s_->types().is_void(s_->types().pointee(r.type));
      if (r.base == interp::Base::global || erased) {
        for (ir::PathStep step : r.path) {
          if (step.step != ir::Step::elem)
            return {std::nullopt, reason_arg({interp::Reason::unsupported_constant, 0, {}})};
          path.push_back(step.index);
        }
      }
      return {vs.address(r.type, base, r.offset, path), 0, true};
    }
    }
    return {};
  }

  // A member of a closure of the part being analysed, which is not committed yet
  // (docs/design/closures.md, decision 12): its L1 from the drafts, committed now.
  std::optional<interp::Callee> closure_callee(EntityId f) {
    for (std::size_t i = 0; i < s_->closures.size(); ++i) {
      const ast::ClosureDef& c = s_->closures[i];
      if (c.call != f && c.invoker != f) continue;
      if (c.flags & closure_flag::polymorphic_result)
        return interp::Callee{.is_constexpr = true, .unsupported = true};
      std::vector<ast::LocalRec> locals = s_->closure_locals[i];
      for (ast::LocalRec& l : locals) l.decl = NodeId::none;  // drafts
      ast::Child body = s_->builder().commit(ast::Child{c.call == f ? c.body : c.invoker_body, 0});
      Sema::ClosureMember m = Sema::closure_member_def(s_->world(), c, f, body.id, std::move(locals));
      irgen::FunctionInput input{f, m.decl, m.body, s_->item(), s_->loc_base() + c.body_begin, false, {}, m.this_type};
      EvalFacts facts(s_->world(), *q_);
      interp::Callee r;
      r.is_constexpr = true;
      auto ir = irgen::function_ir(s_->ins(), input, facts);
      if (!ir) {
        r.unsupported = true;
        return r;
      }
      owned_.push_back(std::move(*ir));
      r.body = &owned_.back().body;
      return r;
    }
    return std::nullopt;
  }

  Sema* s_;
  Questions* q_;
  std::deque<irgen::FunctionIr> owned_;  // the bodies `closure_callee` made, for the evaluation
};

}  // namespace ycxx::sema
