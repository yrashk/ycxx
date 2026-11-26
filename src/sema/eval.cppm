// Constant evaluation in sema ([expr.const]; query-engine.md §B7; DECISIONS §4.6): an analysed
// expression becomes the L1 body of a thunk, `{ return e; }`, through `function_ir`, and the
// interpreter (ycxx.interp) runs it in constant mode inside the query that analyses the
// expression. Callees' bodies and namespace-scope variables come through `Questions`.
//
// The thunk imports the enclosing function's locals usable in constant expressions with their
// known values. Supported by-value record parameters and direct named automatic receivers can
// also supply an object identity while their contents remain unavailable to constant evaluation.
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
    potentially_overlapping_field = [&q](EntityId c, std::uint32_t field) {
      const ClassRec* r = q.classes().find(c);
      return r && field < r->fields.size() && r->fields[field].no_unique_address;
    };
  }
  static constexpr bool evaluating_constants = true;
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
  // A demanded specialization already diagnosed a type trait at its original spelling.
  bool type_trait_failure = false;
  // The global that caused this evaluation to fail already diagnosed its initializer.
  bool variable_instantiation_failure = false;
  // The selected callee already diagnosed its body.
  bool function_instantiation_failure = false;

  bool diagnosed_failure() const {
    return type_trait_failure || variable_instantiation_failure || function_instantiation_failure;
  }
};

class Evaluator {
  struct ByteEvaluation {
    irgen::ConstantByteCapture capture;
    interp::ByteCapture result{ir::NodeId{ir::none}};
  };

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

  // The two accessors see the same receiver, including its original cv and value category.
  std::optional<std::string> evaluate_bytes(Ex receiver) {
    if (!receiver.ok()) return std::nullopt;
    if (!receiver.glvalue()) receiver = s_->materialize(receiver, receiver.type(), false);
    if (!receiver.ok()) return std::nullopt;
    struct RestoreLocals {
      Sema& sema;
      std::size_t first;
      ~RestoreLocals() { sema.discard_local_declarations(first); }
    } restore{*s_, s_->locals().size()};
    const u32 at = receiver.begin;
    const u32 local = s_->declare_local(s_->no_name(), receiver.type(), 0, at);
    Ex object = s_->node(ast::LocalRef{receiver.info, local}, at, receiver.end, receiver.info);
    auto accessor = [&](std::string_view name, QualType type) {
      Ex callee = s_->member(object, false, s_->ins().names.intern(name), receiver.end, at, at);
      if (!callee.ok()) return callee;
      return s_->convert(s_->call(callee, {}, receiver.end, at), type, at);
    };
    Ex size = accessor("size", s_->builtin(s_->world().target.size_type));
    if (!size.ok()) return std::nullopt;
    const auto character = s_->types().add_cv(s_->builtin(Builtin::char_), Cv::const_);
    Ex data = accessor("data", s_->types().pointer(character));
    if (!data.ok()) return std::nullopt;

    auto commit = [&](Ex e) { return s_->builder().commit(Sema::at(e, 0)); };
    ByteEvaluation bytes{{commit(receiver), commit(size), commit(data), local}};
    const Evaluated value = evaluate(s_->void_prvalue(at, receiver.end), false, {}, &bytes);
    if (!value.constant || !bytes.result.bytes) {
      if (!value.type_trait_failure && !(value.dependent && q_->instantiation_gave_up()))
        s_->diag(at, Diag::not_constant, value.reason);
      return std::nullopt;
    }
    return std::move(bytes.result.bytes);
  }

private:
  // Evaluates `e` as a core constant expression, then checks the result is a permitted result
  // ([expr.const]). The result for a reference initializer is a pointer to the bound object.
  Evaluated evaluate(Ex e, bool reference_initializer,
                     irgen::FunctionInput::InitializedFields initialized_fields = {}, ByteEvaluation* bytes = nullptr) {
    if (!e.ok()) return {};
    if (s_->dependent(e)) return {std::nullopt, reason_unsupported, false, true};
    if (!reference_initializer)
      if (auto value = tag_initializer_constant(e)) return {*value, 0, true};
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
    NodeId object_pointer = NodeId::none;
    bool virtual_receiver = false;
    if (const auto member = s_->as<ast::MemberCall>(e)) object_pointer = member->object.id;
    if (const auto call = s_->as<ast::ImmediateVirtualCall>(e)) {
      object_pointer = call->object.id;
      virtual_receiver = true;
    }
    if (const auto call = s_->as<ast::VirtualMemberCall>(e);
        call && in.seqs.at(call->selection, 0).function != EntityId::none) {
      object_pointer = call->object.id;
      virtual_receiver = true;
    }
    const auto receiver = direct_local_receiver(object_pointer);
    ast::Child root = s_->builder().commit(ast::Child{e.id, 0});
    if (reference_initializer && irgen::materializes_temporary(in.ast, root.id))
      return {std::nullopt, reason_arg({interp::Reason::local_address, 0, {}})};
    NodeId ret = in.intern(ast::Return{root}, e.width());
    std::vector<ast::LocalRec> locals(s_->locals().begin(), s_->locals().end());
    auto local_values = s_->local_values();
    std::vector<std::optional<ConstId>> filtered_values;
    std::vector<u32> opaque_record_locals;
    std::vector<u32> discardable_reference_parameters;
    for (std::size_t i = 0; i < locals.size(); ++i) {
      auto& l = locals[i];
      const bool parameter = (l.flags & local_flag::parameter) &&
                             !(l.flags & (ast::local_flag::requires_parameter | local_flag::default_argument_parameter));
      if (s_->types().is_class(l.type)) {
        if (i < local_values.size() && local_values[i]) {
          if (!importable_local_record(l.type, local_values[i])) {
            if (filtered_values.empty()) filtered_values.assign(local_values.begin(), local_values.end());
            filtered_values[i].reset();
          }
        } else if (s_->in_function_body) {
          const bool named_receiver = receiver == i && l.entity == EntityId::none && l.name != s_->no_name() &&
                                      !(l.flags & ast::local_flag::structured_binding) &&
                                      s_->as<ast::LocalVar>({l.decl}).has_value();
          if ((parameter || named_receiver) &&
              (importable_local_record(l.type, std::nullopt) ||
               (virtual_receiver && receiver == i && opaque_dynamic_record(l.type))))
            opaque_record_locals.push_back(static_cast<u32>(i));
        }
      } else if (parameter && s_->in_function_or_lambda_body() && l.name != s_->no_name() &&
                 s_->types().is_reference(l.type) && s_->types().is_class(s_->types().referee(l.type))) {
        discardable_reference_parameters.push_back(static_cast<u32>(i));
        if (virtual_receiver && receiver == i && opaque_dynamic_record(s_->types().referee(l.type)))
          opaque_record_locals.push_back(static_cast<u32>(i));
      }
      l.decl = NodeId::none;  // drafts of the enclosing frame
      // In a template's definition a local of dependent type has no object yet; a non-dependent
      // expression cannot name it, so it stands as an `int` outside its lifetime.
      // So does a variable whose initializer is being read to deduce its placeholder type.
      if (s_->types().is_dependent(l.type) || s_->types().has_placeholder(l.type)) l.type = s_->builtin(Builtin::int_);
    }
    if (!filtered_values.empty()) local_values = filtered_values;
    ast::DeclSema thunk{};
    thunk.entity = EntityId::none;
    thunk.type = s_->types().function(Types::unqualified(e.type()), {}, false, LangLinkage::cxx);
    thunk.params = in.intern_seq(std::span<const ast::ParamSema>{});
    thunk.template_params = in.intern_seq(std::span<const ast::TemplateParamSema>{});
    irgen::FunctionInput input{EntityId::none, thunk,
                               ast::BodySema{ret, in.intern_seq(std::span<const ast::LocalRec>(locals)),
                                             in.intern_seq(std::span<const ast::ExpansionRec>{}), in.intern_seq(std::span<const u32>{}),
                                             ast::QualType{}, 0, in.intern_seq(std::span<const ast::ClosureDef>{})},
                               s_->item(), 0, true, local_values};
    input.initialized_fields = initialized_fields;
    input.opaque_record_locals = opaque_record_locals;
    input.discardable_reference_parameters = discardable_reference_parameters;
    EvalFacts facts(s_->world(), *q_);
    auto body = bytes ? irgen::constant_byte_capture_ir(in, input, facts, bytes->capture) : irgen::function_ir(in, input, facts);
    if (!body) return {};
    if (bytes) bytes->result.site = bytes->capture.site;
    Host host{q_, this};
    interp::Config config{
        .enum_layout = [&facts](EntityId e) { return facts.constant_enum_layout(e); },
        .record_layout = [this](QualType t) { return evaluation_record_layout(t); },
        .virtual_target = [this](EntityId selected, EntityId receiver, EntityId dynamic, bool immediate) {
          return virtual_target(selected, receiver, dynamic, immediate);
        }};
    interp::Outcome out = interp::evaluate(host, in, s_->world().target, body->body, EntityId::none, {}, std::move(config),
                                            bytes ? &bytes->result : nullptr).run();
    if (out.status != interp::Outcome::Status::value) {
      Evaluated result{std::nullopt};
      result.variable_instantiation_failure = out.failure.global != EntityId::none &&
                                              q_->variable_instantiation_failed(out.failure.global);
      result.type_trait_failure = host.type_trait_failure;
      result.function_instantiation_failure = host.function_instantiation_failure;
      if (result.type_trait_failure) return result;
      if (q_->instantiation_gave_up()) {
        result.dependent = true;
        return result;
      }
      interp::Failure f = out.failure;
      // A missing interpreter operation may hide a binding that requires static initialization.
      if (reference_initializer && (f.reason == interp::Reason::unsupported || f.reason == interp::Reason::unsupported_constant ||
                                    f.reason == interp::Reason::callee_unsupported))
        return result;
      // A selected default can also read a field before its lifetime has begun.
      if (f.reason == interp::Reason::outside_lifetime && initialized_fields.owner == EntityId::none &&
          !f.stack.empty() && f.stack[0].function == EntityId::none)
        f.reason = interp::Reason::not_usable;
      result.reason = reason_arg(f);
      return result;
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
  // L1 can materialize this array shape as a field of a constant record.
  static bool constant_record_char_array(Types& types, QualType type) {
    if (!types.is_array(type)) return false;
    const TypeRec array = types.rec(type);
    const QualType element = types.canonical(array.inner);
    return array.bound_value != ConstId::none && !Types::is_volatile(element) && types.is(element, Builtin::char_);
  }

  // An aggregate initializer is constant when each of its elements is constant ([expr.const]).
  Evaluated evaluate_initializer(Ex e, bool zero_default = true) {
    return evaluate_initializer(e, zero_default, {});
  }

private:
  Evaluated evaluate_initializer(Ex e, bool zero_default, irgen::FunctionInput::InitializedFields initialized_fields) {
    if (auto value = tag_initializer_constant(e)) return {*value, 0, true};
    if (const auto literal = s_->as<ast::Literal>(e);
        literal && s_->types().is_array(e.type()) && s_->ins().values.is<ast::VAggregate>(literal->value))
      return {literal->value, 0, true};
    if (s_->as<ast::InitList>(e)) {
      const auto clauses = s_->list_elements(e);
      EntityId owner = EntityId::none;
      if (std::ranges::any_of(clauses, [&](Ex clause) { return s_->as<ast::DefaultMemberInit>(clause).has_value(); }) &&
          constant_record_layout(s_->types().add_cv(e.type(), Cv::const_)))
        owner = s_->types().class_of(e.type());
      std::vector<ConstId> elements;
      elements.reserve(clauses.size());
      for (const Ex& element : clauses) {
        irgen::FunctionInput::InitializedFields fields;
        if (owner != EntityId::none && s_->as<ast::DefaultMemberInit>(element)) fields = {owner, elements};
        Evaluated result = evaluate_initializer(element, zero_default, fields);
        if (!result.value) return result;
        elements.push_back(*result.value);
      }
      return {s_->world().values.aggregate(e.type(), elements), 0, true};
    }
    if (auto dmi = s_->as<ast::DefaultMemberInit>(e))
      return evaluate_initializer({dmi->init.id, e.begin, e.end, e.info}, zero_default, initialized_fields);
    if (s_->as<ast::ValueInit>(e) || (zero_default && s_->as<ast::DefaultInit>(e))) {
      if (auto value = zero_constant(e.type())) return {*value, 0, true};
      return {};
    }
    if (s_->types().is_class(e.type()) && !constant_record_layout(e.type())) return {};
    return evaluate(e, false, initialized_fields);
  }

public:
  // [dcl.attr.annotation], [meta.reflection.result]: reflect_constant takes its operand by value
  // and requires a valid constant template argument of the deduced structural type.
  std::optional<ConstId> evaluate_annotation(Ex expression, QualType type) {
    if (s_->dependent(expression)) {
      s_->diag(expression.begin, Diag::annotation_not_supported);
      return std::nullopt;
    }
    bool supported = true;
    if (!structural_annotation_type(type, supported)) {
      s_->diag(expression.begin, Diag::annotation_invalid_type);
      return std::nullopt;
    }
    if (!supported || (s_->types().is_class(type) && !s_->is_aggregate(type))) {
      s_->diag(expression.begin, Diag::annotation_not_supported);
      return std::nullopt;
    }
    while (auto paren = s_->as<ast::Paren>(expression)) {
      expression = {paren->operand.id, expression.begin + paren->operand.at, expression.end, paren->info};
    }
    const Evaluated result = evaluate_initializer(expression);
    if (!result.value) {
      if (!result.type_trait_failure) s_->diag(expression.begin, Diag::not_constant, result.reason);
      return std::nullopt;
    }
    if (!annotation_argument_value(*result.value)) {
      s_->diag(expression.begin, Diag::annotation_invalid_type);
      return std::nullopt;
    }
    return result.value;
  }

private:
  // [temp.param] requires structural members of a literal class. The aggregate evaluator
  // stores element values. It has no active-union-member or reference identity and does not
  // evaluate a written destructor.
  bool structural_annotation_type(QualType type, bool& supported) {
    auto& types = s_->types();
    if (Types::is_volatile(types.canonical(type))) return false;
    if (types.is_scalar(type)) return true;
    if (types.is_reference(type)) {
      supported = false;
      return !types.rec(type).rvalue_ref;
    }
    if (types.is_array(type)) return structural_annotation_type(types.rec(type).inner, supported);
    if (!types.is_class(type)) return false;
    const ClassRec* record = s_->class_rec(types.class_of(type));
    if (!record || record->polymorphic || !record->destructor_usable || record->copy_deleted) return false;
    if (record->destructor != EntityId::none && !q_->is_constexpr(record->destructor)) return false;
    supported &= !record->union_ && record->destructor == EntityId::none;
    return std::ranges::all_of(record->bases, [&](const BaseRec& base) {
      return base.access == ast::Access::public_ &&
             structural_annotation_type(types.class_type(base.cls), supported);
    }) && std::ranges::all_of(record->fields, [&](const FieldRec& field) {
      return record->access_of(field.entity) == ast::Access::public_ && !field.mutable_ &&
             structural_annotation_type(field.type, supported);
    });
  }

  bool annotation_argument_value(ConstId value) {
    const auto& values = s_->ins().values;
    if (values.is<ast::VLvalue>(value)) {
      const auto address = values.get<ast::VLvalue>(value);
      // A constituent pointer cannot refer to a string literal or to an immediate function
      // ([temp.arg.nontype], [expr.const]).
      if (values.is<ast::VString>(address.base)) return false;
      if (values.is<ast::VEntity>(address.base))
        return !q_->is_immediate_function(values.get<ast::VEntity>(address.base).entity);
      return true;
    }
    if (values.is<ast::VAggregate>(value)) {
      const auto aggregate = values.get<ast::VAggregate>(value);
      return std::ranges::all_of(s_->ins().seqs.elements(aggregate.elems),
                                 [this](ConstId element) { return annotation_argument_value(element); });
    }
    return true;
  }

  std::optional<ConstId> tag_initializer_constant(Ex e) {
    if (!s_->is_defaulted_tag_initializer(e)) return std::nullopt;
    // The empty record value describes storage for the non-aggregate tag. Sema has already
    // selected its constructor, including the checks for access and explicitness.
    return s_->world().values.aggregate(e.type(), {});
  }

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

  // Use the same field order and layout as initialization and native member access.
  std::optional<interp::ConstantRecordLayout> constant_record_layout(QualType type) {
    Types& types = s_->types();
    type = types.canonical(type);
    if (!types.is_class(type) || Types::is_volatile(type)) return std::nullopt;
    const EntityId entity = types.class_of(type);
    const ClassRec* record = s_->class_rec(entity);
    // An aggregate, a closure type, whose captures its lambda-expression initializes
    // ([expr.prim.lambda.capture]/15), or a class a constructor initializes ([class.ctor]): its
    // fields are its value whatever their access.
    const bool aggregate = s_->is_aggregate(type);
    if (!record || (!aggregate && !record->closure && (!record->user_ctors || record->polymorphic)) || record->union_ ||
        !record->bases.empty() || record->unapplied_layout || q_->class_layout_uncertain(entity) ||
        !record->destructor_usable || record->destructor != EntityId::none)
      return std::nullopt;
    const auto layout = s_->class_layout(entity);
    if (!layout) return std::nullopt;

    interp::ConstantRecordLayout result{layout->size, {}};
    result.fields.reserve(record->fields.size());
    for (std::size_t i = 0; i < record->fields.size(); ++i) {
      const FieldRec& field = record->fields[i];
      if ((aggregate && record->access_of(field.entity) != ast::Access::public_) || field.bit_width || field.no_unique_address ||
          !constant_record_field(field.type))
        return std::nullopt;
      // Preserve the enum type in the field path. The interpreter decodes its underlying value.
      result.fields.push_back({s_->field_access_type(field, type), layout->offsets[i], field.mutable_});
    }
    return result;
  }

  // Dynamic layouts are available only for live interpreter storage. A stored record
  // constant cannot represent a polymorphic object or its base subobject.
  std::optional<interp::ConstantRecordLayout> evaluation_record_layout(QualType type) {
    Types& types = s_->types();
    type = types.canonical(type);
    if (!types.is_class(type) || Types::is_volatile(type)) return std::nullopt;
    const EntityId entity = types.class_of(type);
    const ClassRec* record = s_->class_rec(entity);
    if (!record || !record->dynamic) return constant_record_layout(type);
    if (!q_->classes().constexpr_dynamic_default(entity) || q_->class_layout_uncertain(entity)) return std::nullopt;
    const auto layout = s_->class_layout(entity);
    if (!layout) return std::nullopt;
    interp::ConstantRecordLayout result{.size = layout->size, .dynamic = true};
    if (!record->bases.empty()) {
      const EntityId base = record->bases.front().cls;
      const auto base_layout = s_->class_layout(base);
      if (!base_layout || q_->class_layout_uncertain(base)) return std::nullopt;
      const QualType base_type = types.add_cv(types.class_type(base), Types::cv(type));
      result.primary_base = interp::ConstantRecordBase{base, base_type, layout->base_offsets.front(), base_layout->size};
    }
    return result;
  }

  bool opaque_dynamic_record(QualType type) {
    const auto layout = evaluation_record_layout(type);
    return layout && layout->dynamic;
  }

  // The direct-base path is checked by the interpreter. Select by the override
  // edge recorded at class completion, in the context evaluating this call.
  std::optional<interp::VirtualTarget> virtual_target(EntityId selected, EntityId receiver, EntityId dynamic,
                                                      bool immediate) {
    const auto* declaration = s_->member_decl(selected);
    const auto* type = s_->class_rec(dynamic);
    const auto& entities = s_->world().entities;
    if (!declaration || !declaration->is_virtual || declaration->is_consteval != immediate ||
        entities.rec(selected).scope != receiver || !type)
      return std::nullopt;
    if (!immediate) {
      // The ordinary slice uses namespace-scope classes admitted by dynamic layout.
      const auto namespace_class = [&](EntityId cls) {
        const EntityId scope = entities.rec(cls).scope;
        return scope == EntityId::none || entities.rec(scope).kind == EntityKind::namespace_;
      };
      if (!namespace_class(receiver) || !namespace_class(dynamic)) return std::nullopt;
    }
    if (dynamic != receiver) {
      if (type->bases.size() != 1 || type->bases.front().cls != receiver) return std::nullopt;
      const auto override = std::ranges::find(type->members, selected, &MemberDeclRec::overridden_virtual);
      if (override != type->members.end()) {
        if (override->is_consteval != immediate || override->is_pure) return std::nullopt;
        return interp::VirtualTarget{override->entity, dynamic, immediate || q_->is_constexpr(override->entity)};
      }
    }
    if (declaration->is_pure) return std::nullopt;
    return interp::VirtualTarget{selected, receiver, immediate || q_->is_constexpr(selected)};
  }

  // An ordinary dot call passes the address of its receiver. Import only that named
  // local, without evaluating its declaration or allocating slots for unrelated locals.
  std::optional<u32> direct_local_receiver(NodeId object_pointer) {
    if (object_pointer == NodeId::none) return std::nullopt;
    const auto pointer = s_->as<ast::Unary>({object_pointer});
    if (!pointer || pointer->op != ast::UnaryOp::address_of) return std::nullopt;
    Ex object{pointer->operand.id};
    while (const auto paren = s_->as<ast::Paren>(object)) object.id = paren->operand.id;
    if (const auto local = s_->as<ast::LocalRef>(object)) return local->local;
    return std::nullopt;
  }

  // A thunk can import addresses for supported flat records and known integral field values.
  // An immutable descriptor is also available through the host when importing a known pointer.
  bool importable_local_record(QualType type, std::optional<ConstId> value) {
    const auto& in = s_->ins();
    if (value && !in.values.is<ast::VAggregate>(*value)) return false;
    const auto layout = constant_record_layout(type);
    if (!layout) return false;
    const auto integral = [&](const auto& field) {
      return s_->types().is_integral(field.type) || s_->types().is_enum(field.type);
    };
    if (!value) return std::ranges::all_of(layout->fields, integral);
    const auto elements = in.seqs.elements(in.values.get<ast::VAggregate>(*value).elems);
    if (elements.size() != layout->fields.size()) return false;
    for (std::size_t i = 0; i < elements.size(); ++i) {
      const auto& field = layout->fields[i];
      const ConstId element = elements[i];
      if (integral(field) && in.values.is<ast::VInt>(element)) continue;
      if (!s_->types().is_pointer(field.type) || !in.values.is<ast::VLvalue>(element)) return false;
      const auto address = in.values.get<ast::VLvalue>(element);
      if (address.offset != 0 || in.seqs.size(address.path) || !in.values.is<ast::VEntity>(address.base)) return false;
      const auto* object = s_->world().entities.find(in.values.get<ast::VEntity>(address.base).entity);
      if (!object || object->constant_object == ConstId::none) return false;
    }
    return true;
  }

  bool constant_record_field(QualType type) {
    Types& types = s_->types();
    type = types.canonical(type);
    if (Types::is_volatile(type)) return false;
    if (types.is_array(type)) return constant_record_char_array(types, type);
    if (types.is_pointer(type)) return true;
    if (types.is_enum(type)) {
      const EnumRec* enumeration = s_->enum_rec(types.enum_of(type));
      if (!enumeration || !enumeration->underlying) return false;
      type = *enumeration->underlying;
    }
    const auto b = types.builtin_of(type);
    if (!b) return false;
    const auto& target = s_->world().target;
    return *b == Builtin::nullptr_t || (ast::is_integral(*b) && ast::width_of(target, *b) <= 128) ||
           (ast::is_floating(*b) && ast::computes(target[*b].format));
  }

  struct Host {
    Questions* q;
    Evaluator* e;
    bool type_trait_failure = false;
    bool function_instantiation_failure = false;
    interp::ready<interp::Callee> body(EntityId f) {
      if (auto c = e->closure_callee(f)) return {*c};
      bool diagnosed = false;
      auto callee = q->function_ir(f, diagnosed);
      function_instantiation_failure |= diagnosed;
      return {callee};
    }
    interp::ready<std::optional<interp::GlobalVar>> global(EntityId v) {
      auto value = q->global(v);
      type_trait_failure |= q->type_trait_instantiation_failed(v);
      if (value && value->usable && value->value && e->s_->ins().values.is<ast::VAggregate>(*value->value)) {
        // Block-scope statics also have stored initializers. Const qualification alone does
        // not make a class variable potentially constant ([expr.const]).
        if (Types::is_const(e->s_->types().canonical(value->type)) &&
            e->s_->world().entities.rec(v).linkage != ast::Linkage::none)
          value->record = e->constant_record_layout(value->type);
      }
      return {std::move(value)};
    }
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
      return {vs.integer(t, ast::Bits128{r.bits, r.hi}), 0, true};
    }
    case K::floating: return {ast::float_value(s_->ins(), s_->world().target, r.type, ast::Bits128{r.bits, 0}), 0, true};
    case K::null: return {vs.null_pointer(r.type), 0, true};
    case K::function: return {vs.address(r.type, vs.entity(r.entity), 0), 0, true};
    case K::aggregate: {
      std::vector<ConstId> elements;
      elements.reserve(r.elements.size());
      for (const auto& element : r.elements) {
        const Evaluated value = constant(element);
        if (!value.value) return value;
        elements.push_back(*value.value);
      }
      return {vs.aggregate(r.type, elements), 0, true};
    }
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
