// Module implementation unit for ycxx.sema (Sema member function definitions).
// Kept out of actions.cppm so routine edits do not regenerate the ycxx.sema BMI.
module ycxx.sema;

import std;
import ycxx.ast;
import ycxx.engine;
import ycxx.ir;
import ycxx.interp;
import ycxx.lex;

namespace ycxx::sema {

auto Sema::exact_conversion_call(Ex object, QualType target, u32 at_, bool allow_explicit) -> std::optional<Ex> {
  const EntityId cls = types().class_of(object.type());
  const NameId name = ins().names.intern(lex::conversion_function_name);
  const ClassRec* record = class_rec(cls);
  if (!record || !record->bases.empty()) return std::nullopt;
  auto candidates = member_functions(cls, name);
  const bool lvalue = object.cat() == ValueCategory::lvalue;
  // [over.match.conv]: explicit results must match these value types up to qualification.
  // Keep the existing conservative handling of pointer qualification and reference results
  // that name the target type.
  const bool scalar_value_target = types().is_arithmetic(target) || types().is_enum(target) ||
                                   types().is(target, Builtin::nullptr_t);
  std::erase_if(candidates, [&](const MemberDeclRec* candidate) {
    if (!allow_explicit && candidate->is_explicit) return true;
    const TypeRec fn = types().rec(candidate->type);
    if (scalar_value_target && candidate->is_explicit) {
      const QualType result = types().is_reference(fn.inner) ? types().referee(fn.inner) : fn.inner;
      if (!same_unqualified(result, target)) return true;
    }
    if (!cv_within(Types::cv(object.type()), fn.fn_cv)) return true;
    if (fn.fn_ref == ast::RefQual::rvalue) return lvalue;
    return fn.fn_ref == ast::RefQual::lvalue && !lvalue && fn.fn_cv != Cv::const_;
  });
  // [over.ics.rank]: for an rvalue, an rvalue reference beats an lvalue
  // reference to the same cv-qualified type.
  auto better_object = [&](const TypeRec& first, const TypeRec& second) {
    return !lvalue && first.fn_cv == second.fn_cv &&
           first.fn_ref == ast::RefQual::rvalue && second.fn_ref == ast::RefQual::lvalue;
  };
  const MemberDeclRec* selected = nullptr;
  for (const MemberDeclRec* candidate : candidates) {
    const TypeRec fn = types().rec(candidate->type);
    if (types().is_reference(fn.inner)) return std::nullopt;
    if (!same_unqualified(fn.inner, target)) continue;
    if (selected) {
      const TypeRec previous = types().rec(selected->type);
      if (better_object(previous, fn)) continue;
      if (!better_object(fn, previous)) return std::nullopt;
    }
    selected = candidate;
  }
  if (!selected) return std::nullopt;
  // An exact result wins a tie on the implicit object. Leave sets with
  // different viable object parameter types to general conversion ranking.
  const TypeRec selected_fn = types().rec(selected->type);
  for (const MemberDeclRec* candidate : candidates) {
    const TypeRec fn = types().rec(candidate->type);
    if ((fn.fn_cv != selected_fn.fn_cv || fn.fn_ref != selected_fn.fn_ref) &&
        !better_object(selected_fn, fn)) return std::nullopt;
  }
  Ex callee = bind_members(cls, {selected}, object, false, object.begin, object.end, at_);
  return member_call(callee, {}, object.end, at_);
}

auto Sema::static_cast_to(Ex e, QualType to, u32 at_, bool named_static_cast) -> Ex {
  if (!e.ok()) return e;
  if (dependent(e) || types().is_dependent(to))
    return dependent_node({e}, e.begin, e.end, types().is_dependent(to) ? std::nullopt : std::optional(Types::unqualified(to)));
  if (address_set(e)) return convert(e, to, at_);  // [over.over]: a cast supplies a target type
  QualType target = Types::unqualified(to);
  if (types().is_void(target)) {
    if (!e.glvalue() && (!require_concrete_call_result(e.type(), e.begin) || !check_destructor_access(e.type(), e.begin)))
      return {NodeId::none, e.begin, e.end};
    // A discarded tag prvalue still denotes a temporary object ([expr.context]/2).
    if (is_defaulted_tag_initializer(e)) e = discarded(e);
    return convert_node(discard_gcc_reinterpretation(e), ast::ConversionKind::to_void, target);  // /6
  }
  if (types().is_reference(to)) {
    const TypeRec ref = types().rec(to);
    if (ref.rvalue_ref && e.cat() == ValueCategory::lvalue && types().is_object(ref.inner)) {
      if (auto cast = cast_lvalue_to_xvalue(e, to, ref.inner, at_)) return *cast;
    }
    const auto downcast_category = ref.rvalue_ref ? ValueCategory::xvalue : ValueCategory::lvalue;
    if (named_static_cast && e.cat() == downcast_category) {
      if (auto cast = cast_base_to_derived(e, ref.inner, /*pointer=*/false, at_)) return *cast;
    }
    return convert(e, to, at_);
  }
  // [expr.static.cast]: a cast to a class can directly initialize it with an explicit constructor.
  if (types().is_class(target) && has_user_ctors(target)) return construct(target, {e}, e.begin, e.end);
  if (types().is_class(target) || types().is_array(target)) return convert(e, to, at_);
  if (named_static_cast && types().is_class(e.type()))
    if (auto cast = exact_conversion_call(e, target, at_, /*allow_explicit=*/true)) return *cast;
  // The conversion function's implicit object is the operand itself, not a copy of it: one
  // that cannot bind it is no candidate on a copy either.
  if (types().is_class(e.type())) {
    if (auto choice = select_conversion(e, target, at_)) return call_conversion(e, *choice, to, at_);
    const ClassRec* source = class_rec(types().class_of(e.type()));
    if (source && !source->closure) return convert(e, to, at_);
  }
  Ex v = rvalue(e);
  if (!v.ok()) return v;
  QualType from = Types::unqualified(v.type());
  if (named_static_cast && types().is_pointer(from) && types().is_pointer(target)) {
    if (auto cast = cast_base_to_derived(v, target, /*pointer=*/true, at_)) return *cast;
  }
  // [expr.static.cast]: a conversion to an enumeration without a fixed underlying type
  // retains the original integral value for the constant evaluator's range check.
  const EnumRec* target_enum = types().is_enum(target) ? enum_rec(types().enum_of(target)) : nullptr;
  if (types().is_enum(target) && types().is_floating(from)) {
    QualType u = underlying_of(target);
    auto c = target_enum && target_enum->fixed && constant_of ? constant_of(v) : std::nullopt;
    if (c && w_->values.rec(*c).kind == ValueKind::floating && types().builtin_of(u)) {
      double x = std::trunc(w_->values.rec(*c).real);
      IntShape sh = shape(w_->target, *types().builtin_of(u));
      double top = std::ldexp(1.0, int(sh.is_signed ? sh.bits - 1 : sh.bits));
      if (sh.is_signed ? x >= -top && x < top : x > -1 && x < top)
        return literal(target, w_->values.integer(u, sh.is_signed ? u64(i64(x)) : u64(x)), ValueCategory::prvalue, v.begin, v.end);
    }
    // An underlying `bool` is reached by [conv.fpint]'s truncation too, through int, not by
    // [conv.bool].
    Ex i = types().is(u, Builtin::bool_) ? arithmetic_conversion(arithmetic_conversion(v, builtin(Builtin::int_)), u)
                                         : arithmetic_conversion(v, u);
    return convert_node(i, ast::ConversionKind::integral, target);
  }
  if (types().is_enum(target) && (types().is_integral(from) || types().is_enum(from))) {
    if (types().canonical(from) == types().canonical(target)) return v;
    if (auto lit = as<ast::Literal>(v); lit && target_enum && target_enum->fixed)
      return literal(target, w_->values.integer(underlying_of(target), ast::int_bits(ins(), lit->value)), ValueCategory::prvalue,
                     v.begin, v.end);
    return convert_node(v, ast::ConversionKind::integral, target);
  }
  if (is_scoped_enum(from) && types().is_arithmetic(target)) {
    if (types().is_floating(target)) v = arithmetic_conversion(v, underlying_of(from));
    return arithmetic_conversion(v, target);
  }
  // /14: a pointer to cv void to a pointer to an object type at least as cv-qualified.
  if (types().is_pointer(from) && types().is_pointer(target) && types().is_void(types().pointee(from)) &&
      types().is_object(types().pointee(target)) && cv_within(object_cv(types().pointee(from)), object_cv(types().pointee(target))))
    return convert_node(v, ast::ConversionKind::pointer, target);
  Ex converted = convert(v, to, at_);
  // An explicit cast to the field's declared type produces a new prvalue without its width.
  if (converted.ok() && converted.info.bit_width)
    return convert_node(converted, ast::ConversionKind::integral, converted.type());
  return converted;
}

auto Sema::dynamic_cast_to(Ex e, QualType to, u32 begin, u32 end) -> Ex {
  const Ex fail{NodeId::none, begin, end};
  if (!e.ok()) return fail;
  const QualType target = Types::unqualified(types().canonical(to));
  if (types().is_pointer(target) && types().is_void(types().pointee(target)))
    return dynamic_cast_to_void(e, to, begin, end);
  const bool pointer = types().is_pointer(target);
  const bool reference = types().is_reference(target);
  if (dependent(e) || types().is_dependent(to)) {
    diag(begin, Diag::not_supported, lex::Tk::kw_dynamic_cast);
    return fail;
  }
  const QualType destination = pointer ? types().pointee(target) : types().referee(target);
  if ((!pointer && !reference) || !types().is_class(destination)) {
    diag(begin, Diag::dynamic_cast_target, to);
    return fail;
  }
  if (!require_complete(destination, begin)) return fail;
  const auto from = e.type();
  // Diagnose non-pointers before rvalue conversion can need a class layout.
  const bool valid_operand = pointer ? types().is_pointer(from) || types().is_array(from)
      : types().is_class(from) && e.glvalue() && (types().rec(target).rvalue_ref || e.cat() == ValueCategory::lvalue);
  if (!valid_operand) {
    diag(begin, Diag::dynamic_cast_operand, from);
    return fail;
  }
  if (pointer) {
    if (types().is_array(from) && !e.glvalue()) e = materialize(e, from, false);
    // A direct, nonvolatile constexpr pointer can supply its constant value
    // in either evaluation mode. Do not speculate on calls or indirect reads.
    if (unevaluated == 0 && e.glvalue() && types().is_pointer(from) && object_cv(from) == Cv::const_) {
      Ex name = e;
      while (auto paren = as<ast::Paren>(name)) name.id = paren->operand.id;
      std::optional<ConstId> value;
      if (auto local = as<ast::LocalRef>(name)) value = local_values_[local->local];
      if (auto decl = as<ast::DeclRef>(name); decl && !decl->reference) value = q_->constant_value(decl->entity);
      if (value && ins().values.is<ast::VNullptr>(*value))
        e = literal(Types::unqualified(types().canonical(from)), *value, ValueCategory::prvalue, e.begin, e.end);
    }
    e = rvalue(e);
  }
  if (!e.ok()) return fail;
  const QualType source = pointer ? types().pointee(e.type()) : e.type();
  if (!types().is_class(source)) {
    diag(begin, Diag::dynamic_cast_operand, from);
    return fail;
  }
  if (!require_complete(source, begin)) return fail;
  if (!cv_within(object_cv(source), object_cv(destination))) {
    diag(begin, Diag::no_conversion, from, target);
    return fail;
  }
  const EntityId src = types().class_of(source);
  const EntityId dst = types().class_of(destination);
  int paths = 0;
  const auto base = src != dst ? base_path(src, dst, paths) : std::nullopt;
  if (src == dst || base) {
    if (pointer) return convert(e, target, begin);
    if (base && !check_base_path(source, dst, base, paths, begin)) return fail;
    diag(begin, Diag::not_supported, lex::Tk::kw_dynamic_cast);
    return fail;
  }
  const auto* record = class_rec(src);
  if (!record || !record->polymorphic) {
    diag(begin, Diag::dynamic_cast_polymorphic, source);
    return fail;
  }
  if (!q_->rtti_enabled()) {
    diag(begin, Diag::rtti_disabled, lex::Tk::kw_dynamic_cast);
    return fail;
  }
  if (!pointer || !w_->target.supports(ast::NativeOperation::peer_typed_dynamic_cast)) {
    diag(begin, Diag::not_supported, lex::Tk::kw_dynamic_cast);
    return fail;
  }
  // The final unit checks the peer's ownership and layout before lowering a runtime cast.
  // In a constant expression the null path can finish without consulting any RTTI.
  if (unevaluated == 0) q_->use_typed_dynamic_cast();
  const ExprInfo info{target, ValueCategory::prvalue, 0};
  return node(ast::DynamicCastPointer{info, type_info_entity(*w_, src), type_info_entity(*w_, dst),
                                      dynamic_cast_runtime(*w_), at(e, begin)}, begin, end, info);
}

auto Sema::dynamic_cast_to_void(Ex e, QualType to, u32 begin, u32 end) -> Ex {
  const Ex fail{NodeId::none, begin, end};
  if (!e.ok()) return fail;
  const QualType target = Types::unqualified(types().canonical(to));
  if (dependent(e) || types().is_dependent(to) || !types().is_pointer(target) ||
      !types().is_void(types().pointee(target))) {
    diag(begin, Diag::not_supported, lex::Tk::kw_dynamic_cast);
    return fail;
  }
  const QualType from = e.type();
  if (!types().is_pointer(from) || !types().is_class(types().pointee(from))) {
    diag(begin, Diag::dynamic_cast_void_operand);
    return fail;
  }
  const QualType source = types().pointee(from);
  if (!require_complete(source, begin)) return fail;
  const EntityId cls = types().class_of(source);
  const ClassRec* record = classes().find(cls);
  if (!record || !record->polymorphic) {
    diag(begin, Diag::dynamic_cast_void_operand);
    return fail;
  }
  if (!cv_within(object_cv(source), object_cv(types().pointee(target)))) {
    diag(begin, Diag::no_conversion, from, target);
    return fail;
  }
  // Reuse the root classifications for the Linux peer ABI. Accessing the address point
  // establishes no other object layout or virtual emission support.
  const auto& entity = w_->entities.rec(cls);
  if (w_->target.triple != ast::x86_64_linux.triple || entity.linkage != ast::Linkage::external ||
      record->has_explicit_alignment() || q_->class_layout_uncertain(cls) ||
      (record->peer_virtual_layout != PeerVirtualLayout::root && !q_->peer_pure_dispatch(cls))) {
    diag(begin, Diag::not_supported, lex::Tk::kw_dynamic_cast);
    return fail;
  }
  e = rvalue(e);
  if (!e.ok()) return fail;
  const ExprInfo info{target, ValueCategory::prvalue, 0};
  return node(ast::DynamicCastVoid{info, at(e, begin)}, begin, end, info);
}

auto Sema::const_cast_to(Ex e, QualType to, u32 at_) -> Ex {
  if (!e.ok()) return e;
  const Ex fail{NodeId::none, e.begin, e.end};
  const TypeRec destination = types().rec(to);
  const bool reference = destination.kind == TypeKind::reference;
  const QualType target = reference ? destination.inner : Types::unqualified(types().canonical(to));
  const ValueCategory category = !reference ? ValueCategory::prvalue :
      destination.rvalue_ref ? ValueCategory::xvalue : ValueCategory::lvalue;
  if (dependent(e) || types().is_dependent(to))
    return dependent_node({e}, e.begin, e.end, types().is_dependent(to) ? std::nullopt : std::optional(target), category);
  const auto invalid = [&] {
    diag(at_, Diag::no_conversion, e.type(), to);
    return fail;
  };
  const auto unsupported = [&] {
    diag(at_, Diag::not_supported, lex::Tk::kw_const_cast);
    return fail;
  };

  // [expr.const.cast]/4: retain the object and change only its glvalue's type/category.
  if (reference) {
    if (!types().is_object(target) || !const_cast_similar(e.type(), target)) return invalid();
    if (e.info.bit_width) {
      diag(at_, Diag::const_cast_bit_field);
      return fail;
    }
    if (!destination.rvalue_ref && e.cat() != ValueCategory::lvalue) return invalid();
    if (!e.glvalue())
      return types().is_class(target) || types().is_array(target) ? unsupported() : invalid();
    if (!types().is_scalar(target) || types().is_member_pointer(target)) return unsupported();
    return convert_node(e, ast::ConversionKind::qualification, target, category);
  }

  // [expr.const.cast]/1,3: the operand of a pointer cast undergoes the value conversions.
  if (types().is_member_pointer(target) && types().is_member_pointer(e.type())) return unsupported();
  if (!types().is_pointer(target) || types().is_function(types().pointee(target))) return invalid();
  if (types().is_array(e.type()) && !e.glvalue())
    return const_cast_similar(types().rec(e.type()).inner, types().pointee(target)) ? unsupported() : invalid();
  if (!types().is_pointer(e.type()) && !types().is_array(e.type())) return invalid();
  Ex value = rvalue(e);
  if (!value.ok()) return fail;
  if (!const_cast_similar(value.type(), target)) return invalid();
  return convert_node(value, ast::ConversionKind::qualification, target);
}

auto Sema::reinterpret_cast_to(Ex e, QualType to, u32 at_) -> Ex {
  if (!e.ok()) return e;
  const Ex fail{NodeId::none, e.begin, e.end};
  const TypeRec destination = types().rec(to);
  const bool reference = destination.kind == TypeKind::reference;
  const QualType target = reference ? destination.inner : Types::unqualified(to);
  const ValueCategory category = !reference ? ValueCategory::prvalue :
      destination.rvalue_ref ? ValueCategory::xvalue : ValueCategory::lvalue;
  if (dependent(e) || types().is_dependent(to))
    return dependent_node({e}, e.begin, e.end, types().is_dependent(to) ? std::nullopt : std::optional(target), category);
  const auto unsupported = [&] {
    diag(at_, Diag::not_supported, lex::Tk::kw_reinterpret_cast);
    return fail;
  };
  const auto invalid = [&](QualType from) {
    diag(at_, Diag::no_conversion, from, to);
    return fail;
  };
  const auto object_pointer = [&](QualType t) {
    if (!types().is_pointer(t)) return false;
    QualType pointee = types().pointee(t);
    return types().is_object(pointee) || types().is_void(pointee);
  };
  const auto function_pointer = [&](QualType t) {
    return types().is_pointer(t) && types().is_function(types().pointee(t));
  };
  const auto pointer_conversion = [&](QualType from, QualType into) {
    return (object_pointer(from) || function_pointer(from)) && (object_pointer(into) || function_pointer(into));
  };
  // GCC folds a cast to the operand's own type, including its cv-qualifiers, into the operand,
  // which constant evaluation then accepts.
  const bool gcc = compatibility() == ast::Compatibility::gcc;

  // /11: a glvalue of type T1 names the same object as a T2 glvalue when a T1 pointer can be
  // reinterpreted as a T2 pointer. The operand keeps its object; no temporary is created.
  if (reference) {
    if (!e.glvalue()) {
      diag(at_, Diag::reinterpret_rvalue_reference, to);
      return fail;
    }
    if (e.info.bit_width) {
      diag(at_, Diag::reinterpret_bit_field);
      return fail;
    }
    if (!types().is_object(target) || !types().is_object(e.type())) return unsupported();
    const QualType from = types().pointer(e.type()), into = types().pointer(target);
    if (casts_away_constness(from, into)) {
      diag(at_, Diag::reinterpret_casts_away_qualifiers, from, into);
      return fail;
    }
    if (gcc && types().canonical(e.type()) == types().canonical(target))
      return convert_node(e, ast::ConversionKind::qualification, target, category);
    return convert_node(e, ast::ConversionKind::reinterpret, target, category);
  }

  Ex v = rvalue(e);
  if (!v.ok()) return v;
  QualType from = Types::unqualified(v.type());
  const bool identity = types().canonical(from) == types().canonical(target);
  // /2: an integral, enumeration or pointer value converts to its own type.
  if (identity && (types().is_integral(target) || types().is_enum(target) || types().is_pointer(target)))
    return gcc ? v : convert_node(v, ast::ConversionKind::reinterpret, target);
  unsigned pointer_bits = w_->target.pointer_size * 8;
  if (object_pointer(from) && types().is_integral(target)) {
    unsigned bits = shape(w_->target, *types().builtin_of(target)).bits;
    if (bits == pointer_bits) return convert_node(v, ast::ConversionKind::pointer_to_integral, target);
    if (bits < pointer_bits) return invalid(from);
  }
  if (types().is_integral(from) && object_pointer(target) &&
      shape(w_->target, *types().builtin_of(from)).bits == pointer_bits)
    return convert_node(v, ast::ConversionKind::integral_to_pointer, target);
  // /6, /7, /8: between object pointers, between function pointers, and (conditionally
  // supported, as on every target ycxx has) between the two.
  if (pointer_conversion(from, target)) {
    if (casts_away_constness(from, target)) {
      diag(at_, Diag::reinterpret_casts_away_qualifiers, from, to);
      return fail;
    }
    return convert_node(v, ast::ConversionKind::reinterpret, target);
  }
  // Valid forms not supported yet: member pointers (/10), std::nullptr_t to an integer (/4),
  // and an enumeration or a narrower or wider integer to a pointer (/5).
  if (types().is_member_pointer(from) || types().is_member_pointer(target) ||
      (types().is(from, Builtin::nullptr_t) && types().is_integral(target)) ||
      ((types().is_integral(from) || types().is_enum(from)) && types().is_pointer(target)))
    return unsupported();
  return invalid(from);
}

auto Sema::new_class_initializer(QualType t, const std::optional<std::vector<Ex>>& args, const Braced* braces, u32 begin, u32 end) -> Ex {
  Ex fail{NodeId::none, begin, end};
  if (braces) {
    if (braces->clauses.empty() && compatibility() == ast::Compatibility::gcc && is_union(t) && is_aggregate(t)) {
      const auto& fields = class_rec(types().class_of(t))->fields;
      // GCC checks the implicit default constructor on this allocation path. A variant with
      // a nontrivial default constructor deletes it when no member has an initializer.
      const bool has_default = std::ranges::any_of(fields, [](const FieldRec& field) {
        return field.dmi || field.init != ConstId::none;
      });
      // A contributing class ends the search, so every previously visited class can be skipped.
      std::set<EntityId> visited;
      if (!has_default && std::ranges::any_of(fields, [&](const FieldRec& field) {
            const QualType element = types().innermost_array_element(field.type);
            const ClassRec* variant = types().is_class(element) ? class_rec(types().class_of(element)) : nullptr;
            // Preserve the immediate variant's exemption. The narrower proof applies below it.
            return variant && !variant->trivial_defaulted_default_ctor &&
                   (gcc_union_variant_prevents_implicit_default(field.type, visited) || default_deleted(field.type));
          })) {
        diag(begin, Diag::deleted_constructor, t);
        return fail;
      }
    }
    return list_init(t, *braces);
  }
  if (args && args->empty()) return value_init(t, end, end);
  if (!args) {
    if (default_deleted(t)) {
      diag(begin, Diag::deleted_constructor, t);
      return fail;
    }
    Ex init = default_init(t, end, end);
    if (init.ok() && Types::is_const(types().canonical(t)) && !const_default_constructible(t)) {
      diag(begin, Diag::new_const_uninitialized, t);
      return fail;
    }
    return init;
  }
  if (std::ranges::any_of(*args, [](const Ex& e) { return !e.ok(); })) return fail;
  if (std::ranges::any_of(*args, [&](const Ex& e) { return is_braced(e); })) {
    diag(begin, Diag::not_supported, lex::Tk::l_brace);  // a braced argument in parentheses (#293)
    return fail;
  }
  const u32 open = args->front().begin;
  const bool aggregate = can_parenthesize_aggregate(t, *args, !immediate_context);
  if (aggregate || has_user_ctors(t) || args->size() != 1)
    return construct(t, *args, open, end, aggregate ? Construction::direct_paren_aggregate : Construction::direct);
  return convert(args->front(), t, open);
}

auto Sema::new_object(QualType t, const std::optional<std::vector<Ex>>& args, const Braced* braces, u32 begin, u32 end, std::optional<Ex> placement) -> Ex {
  Ex fail{NodeId::none, begin, end};
  if (types().has_placeholder(t)) {
    diag(begin, Diag::not_supported, lex::Tk::kw_new);
    return fail;
  }
  if (types().is_dependent(t)) {
    // The instantiation analyses the new-expression's tokens again with T bound ([temp.res]).
    std::vector<Ex> ops;
    if (placement) ops.push_back(*placement);
    if (args) ops.insert(ops.end(), args->begin(), args->end());
    if (braces)
      for (const Braced::Clause& c : braces->clauses)
        if (!c.list) ops.push_back(c.expr);
    if (std::ranges::any_of(ops, [](const Ex& e) { return !e.ok(); })) return fail;
    return dependent_node(ops, begin, end);
  }
  if (!types().is_object(t)) {
    diag(begin, Diag::new_requires_object, t);
    return fail;
  }
  if (types().is_class(t) && !require_complete(t, begin)) return fail;
  if (!require_concrete_object(t, begin)) return fail;
  const bool class_type = types().is_class(t);
  if (class_type && !supported_class_allocation(t)) {
    diag(begin, Diag::not_supported, lex::Tk::kw_new);
    return fail;
  }
  if (!class_type && !scalar_allocation_type(t)) {
    diag(begin, Diag::not_supported, lex::Tk::kw_new);
    return fail;
  }
  if (placement) {
    auto alignment = layout_of(t, true);
    // [expr.new]: this slice supplies no alignment argument for a new-extended alignment.
    if (!alignment || *alignment > 2u * w_->target.pointer_size) {
      diag(begin, Diag::not_supported, lex::Tk::kw_new);
      return fail;
    }
  }

  Ex init;
  if (class_type) {
    init = new_class_initializer(t, args, braces, begin, end);
  } else if (braces) {
    init = list_init(t, *braces);
  } else if (!args) {
    if (Types::is_const(types().canonical(t))) {
      diag(begin, Diag::new_const_uninitialized, t);
      return fail;
    }
    init = default_init(t, end, end);
  } else if (args->empty()) {
    init = value_init(t, end, end);
  } else {
    if (args->size() != 1) {
      diag((*args)[1].begin, Diag::too_many_initializers, t);
      return fail;
    }
    const Ex& value = args->front();
    if (!value.ok()) return fail;
    if (is_braced(value)) {
      diag(value.begin, Diag::braces_for_scalar, t);
      return fail;
    }
    // [dcl.init.general]: direct-initialization of bool from std::nullptr_t yields false.
    init = types().is(t, Builtin::bool_) && types().is(value.type(), Builtin::nullptr_t)
        ? to_bool(value, value.begin) : convert(value, t, value.begin);
  }
  if (!init.ok() || (!placement && ambiguous_allocation(begin, false))) return fail;
  QualType pointer = types().pointer(t);
  if (!placement && dependent(init)) return dependent_node({init}, begin, end, pointer);

  Ex size = size_of(t, false, begin, begin);
  if (!size.ok()) return fail;
  Ex allocation;
  if (placement) {
    const auto name = allocation_name(AllocationFunction::allocate);
    Ex callee = allocation_name_expr(name, begin, begin);
    allocation = call(callee, {size, *placement}, placement->end, placement->end);
    if (!allocation.ok()) return fail;
    if (dependent(allocation)) return dependent_node({allocation, init}, begin, end, pointer);
    const auto selected = as<ast::Call>(allocation);
    if (!selected || selected->function == EntityId::none) {
      diag(begin, Diag::not_supported, lex::Tk::kw_new);
      return fail;
    }
    const auto& f = w_->entities.rec(selected->function);
    if (f.scope != EntityId::none || !w_->matches_reserved_placement_function(name, f.type)) {
      diag(begin, Diag::not_supported, lex::Tk::kw_new);
      return fail;
    }
    if (dependent(init)) return dependent_node({allocation, init}, begin, end, pointer);
  } else {
    allocation = usual_allocation_call(size, begin);
  }
  ExprInfo info{pointer, ValueCategory::prvalue, 0};
  return node(ast::NewObject{info, at(allocation, begin), at(init, begin), placement.has_value()}, begin, end, info);
}

auto Sema::delete_object(Ex e, u32 begin, u32 end) -> Ex {
  Ex fail{NodeId::none, begin, end};
  if (!e.ok()) return fail;
  QualType v = builtin(Builtin::void_);
  if (dependent(e)) return dependent_node({e}, begin, end, v);
  if (types().is_class(e.type()) || types().is_array(e.type())) {
    diag(begin, Diag::not_supported, lex::Tk::kw_delete);  // only pointer operands in this slice
    return fail;
  }
  if (!types().is_pointer(e.type()) || !types().is_object(types().pointee(e.type()))) {
    diag(begin, Diag::delete_requires_pointer, e.type());
    return fail;
  }
  QualType t = types().pointee(e.type());
  // [expr.delete]/5: deleting an object of incomplete class type is ill-formed.
  if (types().is_class(t) && !require_complete(t, begin)) return fail;
  EntityId destructor = EntityId::none;
  if (types().is_class(t)) {
    // A virtual destructor selects the deallocation function of the dynamic type through the
    // deleting destructor (#931, #309).
    const ClassRec* r = class_rec(types().class_of(t));
    if (!r || r->virtual_destructor || !supported_class_allocation(t)) {
      diag(begin, Diag::not_supported, lex::Tk::kw_delete);
      return fail;
    }
    if (!check_destructor_access(t, begin)) return fail;
    destructor = destructor_of(t);
  } else if (!scalar_allocation_type(t)) {
    diag(begin, Diag::not_supported, lex::Tk::kw_delete);
    return fail;
  }
  Ex pointer = rvalue(e);
  if (!pointer.ok()) return fail;
  Ex size = size_of(t, false, begin, begin);
  EntityId f = w_->allocation_function(AllocationFunction::deallocate_sized).entity;
  q_->use_allocation_function(f);
  ExprInfo info{v, ValueCategory::prvalue, 0};
  return node(ast::DeleteObject{info, f, at(pointer, begin), at(size, begin), destructor}, begin, end, info);
}

auto Sema::arithmetic_array_bound(Ex bound, bool clang_wide) -> bool {
  auto integral = [&](const ExprInfo& info) {
    const auto type = types().builtin_of(info.type);
    if (info.category != ValueCategory::prvalue || !type || !ast::is_integral(*type)) return false;
    const auto width = ast::width_of(w_->target, *type);
    return width != 0 && width <= (clang_wide ? 128 : 64);
  };
  std::vector<NodeId> pending{bound.id};
  while (!pending.empty()) {
    const Ex e{pending.back()};
    pending.pop_back();
    if (auto lit = as<ast::Literal>(e)) {
      if (!integral(lit->info) || !ins().values.is<ast::VInt>(lit->value)) return false;
    } else if (auto paren = as<ast::Paren>(e)) {
      if (!integral(paren->info)) return false;
      pending.push_back(paren->operand.id);
    } else if (auto unary = as<ast::Unary>(e)) {
      if (!integral(unary->info) || (unary->op != ast::UnaryOp::plus && unary->op != ast::UnaryOp::minus)) return false;
      pending.push_back(unary->operand.id);
    } else if (auto binary = as<ast::Binary>(e)) {
      using enum ast::BinaryOp;
      if (!integral(binary->info) || (binary->op != add && binary->op != sub && binary->op != mul)) return false;
      pending.push_back(binary->rhs.id);
      pending.push_back(binary->lhs.id);
    } else if (auto shift = as<ast::OrderedBinary>(e)) {
      if (!clang_wide || !integral(shift->info) ||
          (shift->op != ast::OrderedOp::shl && shift->op != ast::OrderedOp::shr)) return false;
      pending.push_back(shift->rhs.id);
      pending.push_back(shift->lhs.id);
    } else if (auto conversion = as<ast::ImplicitConversion>(e)) {
      if (!integral(conversion->info) || conversion->conversion != ast::ConversionKind::integral ||
          conversion->function != EntityId::none) return false;
      pending.push_back(conversion->operand.id);
    } else {
      return false;
    }
  }
  return true;
}

auto Sema::new_array(QualType t, Ex bound, bool literal_arithmetic_bound, const std::optional<std::vector<Ex>>& args, const Braced* braces, u32 begin, u32 end) -> Ex {
  Ex fail{NodeId::none, begin, end};
  if (!bound.ok()) return fail;
  if (!types().is_object(t)) {
    diag(begin, Diag::new_requires_object, t);
    return fail;
  }
  if (types().has_placeholder(t) || (args && !args->empty()) || (braces && !braces->clauses.empty())) {
    diag(begin, Diag::not_supported, lex::Tk::kw_new);
    return fail;
  }
  if (types().is_dependent(t)) {
    // The specialization checks the allocated element type and its initialization on replay.
    Ex count = convert(bound, builtin(w_->target.size_type), bound.begin);
    if (!count.ok()) return fail;
    return dependent_node({count}, begin, end, types().pointer(t));
  }
  if (!require_concrete_object(t, begin)) return fail;
  if (!scalar_allocation_type(t)) {
    diag(begin, Diag::not_supported, lex::Tk::kw_new);
    return fail;
  }
  if (!args && !braces && Types::is_const(types().canonical(t))) {
    diag(begin, Diag::new_const_uninitialized, t);
    return fail;
  }
  const QualType size_type = builtin(w_->target.size_type);
  Ex count = convert(bound, size_type, bound.begin);
  if (!count.ok() || ambiguous_allocation(begin, true)) return fail;
  QualType pointer = types().pointer(t);
  if (dependent(count)) return dependent_node({count}, begin, end, pointer);
  auto size = layout_of(t, false);
  if (!size) {
    diag(begin, Diag::not_supported, lex::Tk::kw_new);
    return fail;
  }
  const auto known = !unevaluated && constant_of ? constant_of(bound) : std::nullopt;
  const auto bound_builtin = types().builtin_of(bound.type());
  const bool clang_wide = compatibility() == ast::Compatibility::clang && known && bound_builtin &&
      ast::is_integral(*bound_builtin) && ast::width_of(w_->target, *bound_builtin) >
      ast::width_of(w_->target, w_->target.size_type);
  auto checked = known;
  if (clang_wide) checked = ast::convert_integral(ins(), w_->target, *known, size_type);
  if (checked && !valid_new_bound(bound, *size, *checked)) return fail;
  if (clang_wide && ast::int_bits(ins(), *known) != ast::int_bits(ins(), *checked))
    diag(bound.begin, Diag::new_bound_conversion, *known, *checked);

  Ex init = braces ? list_init(t, *braces) : args ? value_init(t, end, end) : default_init(t, end, end);
  if (!init.ok()) return fail;
  Child allocation;
  if (!unevaluated && !args && !braces && literal_arithmetic_bound &&
      w_->target.triple == ast::x86_64_linux.triple && arithmetic_array_bound(bound, clang_wide)) {
    const auto written = as<ast::Literal>(bound);
    const ConstId value = written && !clang_wide ? written->value : checked.value_or(ConstId::none);
    if (value != ConstId::none && ins().values.is<ast::VInt>(value)) {
      // Multiply only after checking the constant size selected by this mode.
      const auto integer = ins().values.get<ast::VInt>(value);
      const auto n = integer.bits;
      if (same_unqualified(integer.type, clang_wide ? size_type : bound.type()) &&
          n.hi == 0 && n.lo <= max_new_array_count(*size)) {
        Ex bytes = literal(size_type, w_->values.integer(size_type, n.lo * *size),
                           ValueCategory::prvalue, bound.begin, bound.end);
        allocation = at(usual_allocation_call(bytes, begin, /*array=*/true), begin);
      }
    }
  }
  ExprInfo info{pointer, ValueCategory::prvalue, 0};
  return node(ast::NewArray{info, at(count, begin), allocation, at(init, begin)}, begin, end, info);
}

auto Sema::size_of(QualType t, bool align, u32 begin, u32 end) -> Ex {
  QualType st = builtin(w_->target.size_type);
  if (types().is_dependent(t)) return dependent_node({}, begin, end, st);
  t = types().referee(t);  // [expr.sizeof]/2, [expr.alignof]/3
  const QualType element = types().innermost_array_element(t);
  if (const EnumRec* e = types().is_enum(element) ? q_->defining_enum(types().enum_of(element)) : nullptr;
      e && !e->fixed) {
    diag(begin, Diag::sizeof_incomplete, t);  // [dcl.enum]/6: incomplete until the closing brace
    return {NodeId::none, begin, end};
  }
  if (deferred_enum_layout(element)) return dependent_node({}, begin, end, st);
  if (compatibility() == ast::Compatibility::clang && types().is_class(element)) {
    const ClassRec* record = class_rec(types().class_of(element));
    if (record && record->clang_weak_alignment) {
      u64 reported = record->clang_weak_alignment;
      if (!align) {
        for (TypeRec array = types().rec(t); array.kind == TypeKind::array; array = types().rec(array.inner)) {
          if (array.bound_value == ConstId::none ||
              (array.bound && reported > std::numeric_limits<u64>::max() / array.bound)) {
            diag(begin, Diag::sizeof_incomplete, t);
            return {NodeId::none, begin, end};
          }
          reported *= array.bound;
        }
      }
      return literal(st, w_->values.integer(st, reported), ValueCategory::prvalue, begin, end);
    }
  }
  if (!has_peer_virtual_layout(t) && !require_supported_virtual_layout(t, begin)) return {NodeId::none, begin, end};
  std::optional<u64> n = layout_of(t, align);
  if (!n) {
    if (type_trait_failed(t)) return {NodeId::none, begin, end};
    if (types().is_function(t) || !is_complete(t)) diag(begin, Diag::sizeof_incomplete, t);
    else diag(begin, Diag::not_supported, align ? lex::Tk::kw_alignof : lex::Tk::kw_sizeof);
    return {NodeId::none, begin, end};
  }
  if (checking_class_alignment && types().is_class(element) && q_->class_layout_uncertain(types().class_of(element))) {
    diag(begin, Diag::class_alignment_uncertain_layout);
    return {NodeId::none, begin, end};
  }
  return literal(st, w_->values.integer(st, *n), ValueCategory::prvalue, begin, end);
}

auto Sema::bit_cast_operand(QualType type, u32 at_, bool destination) -> bool {
  const QualType element = types().innermost_array_element(type);
  if (!types().is_object(type) || types().is(type, Builtin::overload_set) ||
      types().is(type, Builtin::bound_member)) {
    diag(at_, Diag::bit_cast_object, type);
    return false;
  }
  if (destination && types().is_array(type)) {
    diag(at_, Diag::bit_cast_array_result, type);
    return false;
  }
  const EnumRec* definition = types().is_enum(element) ? q_->defining_enum(types().enum_of(element)) : nullptr;
  if (!is_complete(type) || (definition && !definition->fixed)) {
    if (!type_trait_failed(type)) diag(at_, Diag::bit_cast_incomplete, type);
    return false;
  }
  if (types().is_class(element)) {
    const auto* record = class_rec(types().class_of(element));
    // Virtual functions and a nontrivial destructor disprove copyability even
    // for a record whose other special-member facts are incomplete ([class.prop]).
    if (record && (record->polymorphic || record->destructor != EntityId::none ||
                   record->trivially_copyable() == false)) {
      diag(at_, Diag::bit_cast_nontrivial, element);
      return false;
    }
    if (destination && record && record->is_abstract()) {
      diag(at_, Diag::abstract_object, type);
      return false;
    }
  }
  return true;
}

auto Sema::bit_cast_representation(QualType type, u64 size) -> bool {
  const auto unsigned_size = [&](QualType t) -> u64 {
    const auto builtin = types().builtin_of(t);
    if (Types::is_volatile(types().canonical(t)) || !builtin || !ast::is_standard_unsigned(*builtin))
      return 0;
    const auto bytes = w_->target[*builtin].size;
    return bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8 ? bytes : 0;
  };
  if (!w_->target.little_endian_unsigned_integers || Types::is_volatile(types().canonical(type))) return false;
  if (!types().is_class(type)) return size != 0 && unsigned_size(type) == size;
  const EntityId cls = types().class_of(type);
  const ClassRec* record = class_rec(cls);
  if (!record || record->union_ || record->polymorphic || !record->bases.empty() ||
      record->trivially_copyable() != true) return false;
  const auto layout = class_layout(cls);
  if (!layout || layout->offsets.size() != record->fields.size()) return false;
  u64 offset = 0;
  for (std::size_t i = 0; i < record->fields.size(); ++i) {
    const FieldRec& field = record->fields[i];
    const u64 bytes = unsigned_size(field.type);
    if (!bytes || field.bit_width || field.no_unique_address || layout->offsets[i] != offset) return false;
    offset += bytes;
  }
  return offset == size;
}

auto Sema::bit_cast(QualType destination, Ex source, u32 begin, u32 end, u32 type_at) -> Ex {
  const Ex fail{NodeId::none, begin, end};
  if (!source.ok()) return fail;
  const QualType from = types().referee(source.type());
  const bool destination_dependent = types().is_dependent(destination);
  const bool source_type_dependent = has_proven_dependent_type(source) || types().is_dependent(from);
  if (!destination_dependent && !bit_cast_operand(destination, type_at, true)) return fail;
  if (!source_type_dependent && !bit_cast_operand(from, source.begin, false)) return fail;
  bool supported = false;
  if (!destination_dependent && !source_type_dependent) {
    const auto to_size = bit_cast_size(destination);
    const auto from_size = bit_cast_size(from);
    if (to_size && from_size) {
      if (*to_size != *from_size) {
        diag(begin, Diag::bit_cast_size, destination, from, *to_size, *from_size);
        return fail;
      }
      supported = !source.info.bit_width && bit_cast_representation(destination, *to_size) &&
                  bit_cast_representation(from, *from_size);
    }
  }
  const bool deferred = destination_dependent || dependent(source);
  if (supported && !deferred && !source.glvalue() && types().is_class(from)) {
    source = materialize(source, from, false);
    if (!source.ok()) return fail;
  }
  const QualType type = types().is_class(destination) || destination_dependent
      ? destination : Types::unqualified(types().canonical(destination));
  u8 flags = 0;
  if (deferred) flags = expr_flag::dependent | (source.info.flags & expr_flag::deferred_decay_copy);
  if (destination_dependent) flags |= expr_flag::type_dependent;
  const ExprInfo info{type, ValueCategory::prvalue, flags};
  return node(ast::BitCast{info, destination, at(source, begin), type_at - begin, supported}, begin, end, info);
}

auto Sema::offset_of(QualType t, NameId name, u32 begin, u32 end, u32 name_at) -> Ex {
  QualType size = builtin(w_->target.size_type);
  if (types().is_dependent(t)) return dependent_node({}, begin, end, size);
  Ex fail{NodeId::none, begin, end};
  EntityId cls = types().is_class(t) ? types().class_of(t) : EntityId::none;
  const ClassRec* record = cls != EntityId::none ? class_rec(cls) : nullptr;
  if (!record) {
    diag(begin, Diag::offsetof_type, t);
    return fail;
  }
  auto member = direct_member(cls, *record, name);
  if (!member || !member->field) {
    diag(name_at, Diag::offsetof_member, name, t);
    return fail;
  }
  if (!check_access(cls, member->field->entity, name, name_at)) return fail;
  if (member->field->bit_width != 0) {
    diag(name_at, Diag::offsetof_bit_field);
    return fail;
  }
  const auto uncertain = q_->uncertain_object_layouts();
  std::set<EntityId> checked;
  auto ordinary = [&](this const auto& self, EntityId c) -> bool {
    if (!checked.insert(c).second) return true;
    const ClassRec* r = class_rec(c);
    if (!r || !r->bases.empty() || r->closure || uncertain.contains(c) || q_->class_layout_uncertain(c)) return false;
    return std::ranges::all_of(r->fields, [&](const FieldRec& f) {
      const QualType field = types().innermost_array_element(f.type);
      return r->access_of(f.entity) == r->access_of(r->fields.front().entity) && !types().is_reference(field) &&
             (!types().is_class(field) || self(types().class_of(field))) &&
             (!types().is_enum(field) || !uncertain.contains(types().enum_of(field)));
    });
  };
  auto layout = ordinary(cls) ? class_layout(cls) : std::nullopt;
  if (!layout) {
    diag(begin, Diag::offsetof_layout, t);
    return fail;
  }
  return literal(size, w_->values.integer(size, layout->offsets[member->index]), ValueCategory::prvalue, begin, end);
}

auto Sema::place_of(QualType t) -> std::optional<ast::Placed> {
  TypeRec r = types().rec(t);
  switch (r.kind) {
  case TypeKind::builtin:
    if (!w_->target.has(r.builtin) || r.builtin == Builtin::void_) return std::nullopt;
    return ast::scalar_placed(w_->target[r.builtin].size, w_->target[r.builtin].align);
  case TypeKind::pointer:
  case TypeKind::reference: return ast::scalar_placed(w_->target.pointer_size, w_->target.pointer_align);  // a reference member's
  case TypeKind::member_pointer: {
    ast::BuiltinLayout l = ast::member_pointer_layout(w_->target, types().is_function(r.inner));
    return ast::scalar_placed(l.size, l.align);
  }
  case TypeKind::enum_: return is_complete(t) && !deferred_enum_layout(t) ? place_of(underlying_of(t)) : std::nullopt;
  case TypeKind::array: {
    if (r.bound_value == ConstId::none) return std::nullopt;
    auto e = place_of(r.inner);
    if (!e) return std::nullopt;
    return ast::array_placed(*e, r.bound);
  }
  case TypeKind::class_: {
    auto l = class_layout(types().class_of(t));
    if (!l) return std::nullopt;
    return l->placed;
  }
  default: return std::nullopt;  // void, functions
  }
}

auto Sema::record_layout(const ClassRec& c, EntityId ce) -> std::optional<ast::RecordLayout> {
  if (c.clang_weak_alignment) return std::nullopt;
  if (c.virtual_layout_unsupported && c.peer_virtual_layout == PeerVirtualLayout::none &&
      c.peer_automatic_primary_base == EntityId::none) return std::nullopt;
  std::vector<ast::Placed> bases, places;
  for (const BaseRec& b : c.bases) {
    if (b.cls == c.peer_automatic_primary_base) {
      bases.push_back(ast::scalar_placed(w_->target.pointer_size, w_->target.pointer_align));
    } else {
      auto m = place_of(types().class_type(b.cls));
      if (!m) return std::nullopt;
      bases.push_back(*m);
    }
  }
  for (const FieldRec& f : c.fields) {
    auto m = place_of(f.type);
    if (!m) return std::nullopt;
    m->bit_width = f.bit_width;
    m->wide_bit_width = f.wide_bit_width;
    if (f.wide_bit_width)
      m->wide_bit_alignment = ast::over_width_bit_field_alignment(w_->target, compatibility(), f.wide_bit_width);
    m->potentially_overlapping = f.no_unique_address;
    m->align = std::max(m->align, u64(f.explicit_alignment));
    places.push_back(*m);
  }
  const bool dynamic_root = c.dynamic && !c.dynamic->primary_base;
  const ast::Target* root = c.peer_virtual_layout == PeerVirtualLayout::root || dynamic_root ? &w_->target : nullptr;
  return ast::record_layout(bases, places, c.union_, c.pod, std::to_underlying(ce), root, c.explicit_alignment);
}

auto Sema::deduce_placeholder(QualType t, Ex e, u32 at_, QualType* replaced) -> std::optional<QualType> {
  if (!e.ok()) return std::nullopt;
  if (dependent(e)) {
    if (replaced) *replaced = builtin(Builtin::dependent);
    return builtin(Builtin::dependent);
  }
  // An explicit template-id can select a single specialization without a target type
  // ([over.over], [temp.deduct.funcaddr]). Keep other overload sets for the diagnostic below.
  if (auto set = address_set(e); set && !is_bound_member(set->name) && explicit_args_.contains(std::to_underlying(set->name.id))) {
    e = resolve_address(e, *set, std::nullopt, at_);
    if (!e.ok()) return std::nullopt;
  }
  // An overload set of functions, none a template: trial deduction from each member whose
  // constraints are satisfied, a result only when they all agree ([temp.deduct.call]/6.2).
  // Members that differ only in their trailing requires-clauses can (#1417).
  std::optional<QualType> overload_argument;
  if (auto set = address_set(e); set && types().kind(t) == TypeKind::placeholder && !set->decls->empty()) {
    bool templates_in_set = false, disagree = false;
    for (const ast::DeclSema& d : *set->decls) {
      if (d.flags & decl_flag::template_) templates_in_set = true;
      if (templates_in_set || !constraints_satisfied(d) || types().has_placeholder(types().rec(d.type).inner)) continue;
      const QualType ft = types().canonical(d.type);
      if (overload_argument && *overload_argument != ft) disagree = true;
      overload_argument = ft;
    }
    if (templates_in_set || disagree) overload_argument.reset();
    if (overload_argument && set->address) overload_argument = types().pointer(*overload_argument);
  }
  if (overload_argument) {
    Templates tm = templates();
    const u16 depth = u16(template_scopes.size());
    Bindings invent;
    invent.placeholder = types().template_param(depth, 0);
    std::vector<std::optional<TemplateArg>> out(1);
    if (auto p0 = tm.substitute(t, invent)) {
      auto [p, a, allow] = tm.call_pair(*p0, ArgShape{*overload_argument, !types().is_pointer(*overload_argument)}, depth);
      if (tm.deduce(p, a, depth, out, allow) && out[0]) {
        Bindings deduced;
        deduced.placeholder = out[0]->type;
        if (replaced) *replaced = out[0]->type;
        if (auto r = tm.substitute(t, deduced)) return *r;
      }
    }
  }
  // Any other overload set, or its address, deduces nothing ([temp.deduct.call]/6); a
  // template-id naming one specialization is not resolved yet (#335).
  if (types().is(e.type(), Builtin::overload_set) || is_bound_member(e) ||
      (types().is_pointer(e.type()) && types().is(types().pointee(e.type()), Builtin::overload_set))) {
    diag(at_, Diag::auto_deduction_failed, t, e.type());
    return std::nullopt;
  }
  if (types().kind(t) == TypeKind::placeholder && types().rec(t).decltype_auto) {
    if (Types::cv(types().canonical(t)) != Cv::none) {  // [dcl.type.auto.deduct]/5: the placeholder alone
      diag(at_, Diag::decltype_auto_alone);
      return std::nullopt;
    }
    // `decltype(auto) x = (y);`: the parenthesized expression's ([dcl.type.auto.deduct]/5).
    QualType r = decltype_of(e, as<ast::Paren>(e).has_value());
    if (replaced) *replaced = r;
    return r;
  }
  if (types_has_decltype_auto(t)) {
    diag(at_, Diag::decltype_auto_alone);
    return std::nullopt;
  }
  // `auto x = f();` of a void `f` deduces void, which the declaration then rejects.
  if (types().is_void(e.type()) && types().kind(t) == TypeKind::placeholder) {
    if (replaced) *replaced = e.type();
    return types().add_cv(e.type(), Types::cv(t));
  }
  if (types().is(e.type(), Builtin::overload_set) || is_bound_member(e) || types().is_void(e.type())) {
    diag(at_, Diag::auto_deduction_failed, t, e.type());
    return std::nullopt;
  }
  Templates tm = templates();
  const u16 depth = u16(template_scopes.size());
  Bindings invent;
  invent.placeholder = types().template_param(depth, 0);
  auto p0 = tm.substitute(t, invent);
  std::vector<std::optional<TemplateArg>> out(1);
  if (p0) {
    auto [p, a, allow] = tm.call_pair(*p0, ArgShape{e.type(), e.cat() == ValueCategory::lvalue}, depth);
    if (tm.deduce(p, a, depth, out, allow) && out[0]) {
      Bindings deduced;
      deduced.placeholder = out[0]->type;
      if (replaced) *replaced = out[0]->type;
      if (auto r = tm.substitute(t, deduced)) return *r;
    }
  }
  diag(at_, Diag::auto_deduction_failed, t, e.type());
  return std::nullopt;
}

auto Sema::array_binding(QualType declared, Ex source, u64 names, u32 at_) -> std::optional<std::pair<QualType, Ex>> {
  if (!source.ok()) return std::nullopt;
  QualType array = source.type();
  if (dependent(source) || !types().is_array(array)) {
    diag(at_, Diag::binding_unsupported_type, array);
    return std::nullopt;
  }
  if (!require_complete(array, at_)) return std::nullopt;
  if (u64 bound = types().rec(array).bound; names != bound) {
    diag(at_, Diag::binding_arity, names, bound);
    return std::nullopt;
  }
  QualType leaf = array;
  while (types().is_array(leaf)) leaf = types().rec(leaf).inner;
  if (!types().is_scalar(leaf) || types().is_member_pointer(leaf)) {
    diag(at_, Diag::binding_unsupported_type, array);
    return std::nullopt;
  }
  if (types().is_reference(declared)) {
    auto ref = deduce_placeholder(declared, source, source.begin);
    if (!ref) return std::nullopt;
    Ex init = bind_reference(source, *ref, source.begin, true);
    if (!init.ok()) return std::nullopt;
    return std::pair{*ref, init};
  }
  const auto original = compatibility() == ast::Compatibility::spec ? object_unqualified(array) : array;
  QualType type = types().add_cv(original, Types::cv(declared));
  if (!source.glvalue()) source = materialize(source, array, false);
  return std::pair{type, convert_node(source, ast::ConversionKind::array_copy, type)};
}

auto Sema::check_binding_lifetime(Ex e, bool unsupported_path) -> bool {
  auto check = [&](Child child, bool unsupported = false) {
    return check_binding_lifetime({child.id, e.begin + child.at, e.end, {}}, unsupported_path || unsupported);
  };
  if (auto m = as<ast::Materialize>(e)) {
    if (unsupported_path) {
      diag(e.begin, Diag::binding_temporary_lifetime);
      return false;
    }
    return !types().is_class(m->info.type) || check_binding_destructor(m->info.type, e.begin);
  }
  if (auto p = as<ast::Paren>(e)) return check(p->operand);
  if (auto m = as<ast::Member>(e); m && !m->arrow && !m->reference) return check(m->object);
  if (auto c = as<ast::ImplicitConversion>(e); c &&
      (c->conversion == ast::ConversionKind::derived_to_base || c->conversion == ast::ConversionKind::base_to_derived ||
       c->conversion == ast::ConversionKind::qualification ||
       (c->conversion == ast::ConversionKind::reinterpret && c->info.category != ValueCategory::prvalue)))
    return check(c->operand);
  if (auto b = as<ast::OrderedBinary>(e)) {
    if (b->op == ast::OrderedOp::dot_star) return check(b->lhs);
    if (b->op == ast::OrderedOp::comma) return check(b->rhs);
  }
  if (auto c = as<ast::Conditional>(e)) return check(c->then_, true) && check(c->else_, true);
  if (auto s = as<ast::Subscript>(e)) {
    Ex base{s->base.id, e.begin + s->base.at, e.end, {}};
    if (auto c = as<ast::ImplicitConversion>(base); c && c->conversion == ast::ConversionKind::array_to_pointer)
      return check_binding_lifetime({c->operand.id, base.begin + c->operand.at, e.end, {}}, true);
  }
  return true;
}

auto Sema::class_binding(QualType declared, const Ex& source, u64 names, u32 at_) -> std::optional<std::pair<QualType, const ClassRec*>> {
  if (!source.ok()) return std::nullopt;
  if (dependent(source)) {
    diag(at_, Diag::binding_unsupported_type, source.type());
    return std::nullopt;
  }
  auto type = deduce_placeholder(declared, source, source.begin);
  if (!type) return std::nullopt;
  QualType object = types().referee(*type);
  if (reject_tuple_binding(object, at_)) return std::nullopt;
  if (!require_complete(object, at_) || !require_supported_virtual_layout(object, at_)) return std::nullopt;
  EntityId entity = types().class_of(object);
  const ClassRec* record = class_rec(entity);
  if (!record || record->union_ || record->closure || record->layout_requires_virtual_support || !record->bases.empty() ||
      w_->entities.rec(entity).templ != EntityId::none) {
    diag(at_, Diag::binding_unsupported_type, object);
    return std::nullopt;
  }
  if (names != record->fields.size()) {
    diag(at_, Diag::binding_member_arity, names, object, u64(record->fields.size()));
    return std::nullopt;
  }
  for (const FieldRec& field : record->fields) {
    if (field.name == no_name()) {
      diag(at_, Diag::binding_unsupported_type, object);
      return std::nullopt;
    }
    if (record->access_of(field.entity) != ast::Access::public_) {
      diag(at_, Diag::binding_member_access, field.name);
      return std::nullopt;
    }
    if (field.bit_width) {
      diag(at_, Diag::binding_bit_field, field.name);
      return std::nullopt;
    }
  }
  if (!types().is_reference(*type) || !source.glvalue()) {
    if (!check_binding_destructor(object, at_)) return std::nullopt;
  } else if (!check_binding_lifetime(source)) {
    return std::nullopt;
  }
  return std::pair{*type, record};
}

auto Sema::to_base(Ex e, EntityId b, u32 at_, bool binding_lvalue_reference, bool check_access) -> Ex {
  if (!e.ok()) return e;
  bool pointer = types().is_pointer(e.type());
  QualType ct = pointer ? types().pointee(e.type()) : e.type();
  Cv cv = Types::cv(ct);
  int paths = 0;
  auto path = base_path(types().class_of(ct), b, paths);
  if (!check_base_path(ct, b, path, paths, at_, check_access)) return {NodeId::none, e.begin, e.end};
  if (unevaluated == 0) {
    const auto* from = class_rec(types().class_of(ct));
    // A marked automatic object's primary-base edge is available for direct reference binding.
    const bool peer_base = from && path->size() == 1 &&
                           ((from->peer_virtual_layout == PeerVirtualLayout::primary_base &&
                             (pointer || peer_object_lvalue(e))) ||
                            (binding_lvalue_reference && from->peer_automatic_primary_base == b &&
                             default_constructed_peer_object(e)));
    if (!peer_base && !require_supported_virtual_layout(ct, at_)) return {NodeId::none, e.begin, e.end};
  }
  for (EntityId step : *path) {
    QualType st = types().add_cv(types().class_type(step), cv);
    ExprInfo info{pointer ? types().pointer(st) : st, pointer ? ValueCategory::prvalue : e.cat(), 0};
    e = node(ast::ImplicitConversion{info, ast::ConversionKind::derived_to_base, step, at(e, e.begin)}, e.begin, e.end, info);
  }
  return e;
}

auto Sema::ordinary_binary_choice(NameId name, std::span<const Ex> operands, std::span<const ast::DeclSema> nonmembers) -> OrdinaryOperatorChoice {
  const NameId conversion = ins().names.intern(lex::conversion_function_name);
  for (const Ex& operand : operands) {
    const QualType type = types().referee(operand.type());
    if (operand.info.bit_width || Types::is_volatile(types().canonical(type))) return {};
    if (types().is_arithmetic(type)) continue;
    if (!types().is_class(type)) return {};
    const EntityId entity = types().class_of(type);
    const auto& owner = w_->entities.rec(entity);
    const auto* record = class_rec(entity);
    if (owner.kind != EntityKind::class_ || owner.templ != EntityId::none || !record || !record->bases.empty() ||
        std::ranges::contains(record->members, conversion, &MemberDeclRec::name))
      return {};
  }
  const Ex& lhs = operands[0];
  const Ex& rhs = operands[1];
  const auto argument_rank = [&](const Ex& operand, QualType parameter) -> std::optional<Ics> {
    const QualType source = types().referee(operand.type());
    const QualType target = types().referee(parameter);
    const bool same_type = same_unqualified(source, target);
    if (types().is_class(target) && !same_type) {
      if (has_user_ctors(target)) return std::nullopt;
      return Ics{};
    }
    // The class-operand check excludes conversion functions and base conversions.
    if (types().is_class(source) && !same_type) return Ics{};
    // Pointer qualification and conversions that bind a reference to a temporary
    // of a different type need additional sequence ordering.
    if ((!same_type && !types().is_arithmetic(target)) ||
        Types::is_volatile(types().canonical(target)) ||
        (types().is_reference(parameter) && !same_type))
      return std::nullopt;
    return ics(operand, parameter);
  };
  struct Ranked {
    std::array<Ics, 2> conversions;
    const ast::DeclSema* nonmember;
  };
  std::vector<Ranked> viable;
  for (const auto& nonmember : nonmembers) {
    const TypeRec function = types().rec(nonmember.type);
    if (!function.variadic && function.params.size() == 1) continue;  // the unary overload
    if ((nonmember.flags & decl_flag::template_) || function.variadic || function.params.size() != 2) return {};
    const auto left = argument_rank(lhs, function.params[0]);
    if (!left) return {};
    if (!left->viable) continue;
    const auto right = argument_rank(rhs, function.params[1]);
    if (!right) return {};
    if (right->viable) viable.push_back({{*left, *right}, &nonmember});
  }
  if (viable.empty()) return {};
  for (const auto* member : member_functions(types().class_of(types().referee(lhs.type())), name)) {
    const TypeRec function = types().rec(member->type);
    if (!function.variadic && function.params.empty()) continue;  // the unary overload
    if (member->is_static || member->is_template || function.variadic || function.params.size() != 1 ||
        function.fn_ref != ast::RefQual::none || (function.fn_cv != Cv::none && function.fn_cv != Cv::const_))
      return {};
    const Ics receiver = implicit_object_rank(function, object_cv(lhs.type()), lhs.cat() != ValueCategory::lvalue);
    if (!receiver.viable) continue;
    const auto right = argument_rank(rhs, function.params[0]);
    if (!right) return {};
    if (right->viable) viable.push_back({{receiver, *right}, nullptr});
  }
  for (const auto& candidate : viable)
    if (std::ranges::all_of(viable, [&](const Ranked& other) {
          return &candidate == &other || better_conversions(candidate.conversions, other.conversions).value_or(false);
        }))
      return {candidate.nonmember};
  return {.ambiguous = true};
}

auto Sema::overloaded(std::string_view op, std::vector<Ex> operands, u32 begin, u32 end, u32 op_at, bool rank_failed_member) -> std::optional<Ex> {
  auto user_type = [&](const Ex& e) {
    QualType t = types().referee(e.type());
    return types().is_class(t) || types().is_enum(t);
  };
  if (std::ranges::none_of(operands, user_type) || any_dependent(operands)) return std::nullopt;
  NameId name = ins().names.intern(std::string("operator") + std::string(op));
  std::size_t n = diag_count();
  std::optional<Ex> ref_qualified_member;
  bool member_only_failure = false;
  const bool binary = operands.size() == 2 && std::ranges::contains(binary_spellings, op);
  QualType lt = types().referee(operands[0].type());
  // [temp.dep.candidate]: an operand that was dependent also finds what is associated with its
  // type at the point of instantiation.
  const bool instantiated = !binary && q_->dependent_call(op_at);
  bool unary_friends = false;
  if (operands.size() == 1 && types().is_class(lt))
    for (const bool at_instantiation : {false, true}) {
      if (at_instantiation && !instantiated) continue;
      const auto associations = adl_associations(operands, at_instantiation, /*ordinary_call=*/!at_instantiation);
      for_each_associated_friend(associations, name, at_instantiation, [&](const ast::DeclSema& d) {
        unary_friends |= types().rec(d.type).params.size() == 1;
      });
    }
  // Compound assignment: rank member and non-member candidates together where
  // `ordinary_binary_choice` models their conversions. Elsewhere the member comes first (#219).
  std::optional<ast::DeclSema> compound_choice;
  if (operands.size() == 2 && !binary && types().is_class(lt) && !member_functions(types().class_of(lt), name).empty()) {
    auto nonmembers = ordinary_operator_nonmembers(name);
    add_operator_adl(nonmembers, name, operands, /*at_instantiation=*/false);
    if (instantiated) add_operator_adl(nonmembers, name, operands, /*at_instantiation=*/true);
    const auto choice = nonmembers.empty() ? OrdinaryOperatorChoice{} : ordinary_binary_choice(name, operands, nonmembers);
    if (choice.ambiguous) {
      diag(op_at, Diag::ambiguous_call, name);
      return Ex{NodeId::none, begin, end};
    }
    if (choice.nonmember) compound_choice = *choice.nonmember;
  }
  if (types().is_class(lt) && !unary_friends && !compound_choice)
    if (const ClassRec* c = class_rec(types().class_of(lt));
        c && std::ranges::any_of(std::array{std::span(c->members), std::span(c->introduced)} | std::views::join,
                                 [&](const MemberDeclRec& m) { return m.name == name && m.is_function && !m.is_static; })) {
      Ex m = member(operands[0], false, name, operands[0].end, op_at, op_at);
      Ex r = m.ok() ? call(m, std::vector<Ex>(operands.begin() + 1, operands.end()), end, op_at) : m;
      if (r.ok()) {
        auto selected = as<ast::MemberCall>(r);
        if (!selected || types().rec(w_->entities.rec(selected->function).type).fn_ref == ast::RefQual::none)
          return Ex{r.id, begin, end, r.info};
        ref_qualified_member = Ex{r.id, begin, end, r.info};
      } else {
        const auto reported = std::span(*diags_).subspan(n);
        if (std::ranges::any_of(reported, [](const auto& d) { return d.id == diag_id(Diag::not_constant); }))
          return Ex{NodeId::none, begin, end};  // an immediate operand conversion failed ([expr.const])
        member_only_failure = std::ranges::any_of(reported, [](const auto& d) {
          return d.id == diag_id(Diag::deleted_function) || d.id == diag_id(Diag::ambiguous_call) ||
                 d.id == diag_id(Diag::private_member) || d.id == diag_id(Diag::protected_member);
        });
        if (member_only_failure && !rank_failed_member) return Ex{NodeId::none, begin, end};
        if (!member_only_failure) truncate_diags(n);
      }
    }
  const bool enum_only = std::ranges::none_of(operands, [&](const Ex& operand) {
    return types().is_class(types().referee(operand.type()));
  });
  auto set = ordinary_operator_nonmembers(name);
  if (compound_choice) {
    set.assign(1, *compound_choice);
  } else if (binary) {
    add_operator_adl(set, name, operands, /*at_instantiation=*/false);
    const bool ordinary_classes = !enum_only && std::ranges::all_of(operands, [&](const Ex& operand) {
      const QualType type = types().referee(operand.type());
      if (!types().is_class(type)) return true;
      const auto& entity = w_->entities.rec(types().class_of(type));
      return entity.kind == EntityKind::class_ && entity.templ == EntityId::none;
    });
    if (ordinary_classes && q_->dependent_call(op_at))
      add_operator_adl(set, name, operands, /*at_instantiation=*/true);
  } else {
    add_operator_adl(set, name, operands, /*at_instantiation=*/false);
    if (instantiated) add_operator_adl(set, name, operands, /*at_instantiation=*/true);
  }
  if (unary_friends) {
    auto found = find_member(types().class_of(lt), name);
    if (found && found->ambiguous) {
      diag(op_at, Diag::ambiguous_member, name, Types::unqualified(lt));
      return Ex{NodeId::none, begin, end};
    }
    struct UnaryCandidate {
      Ics rank;
      QualType target;
      const ast::DeclSema* nonmember;
      bool is_template;
    };
    std::vector<UnaryCandidate> viable;
    Ex object = operands.front();
    auto deduced_type = [&](const ast::DeclSema& pattern, std::span<const ArgShape> shapes,
                            std::span<const NodeId> defaults) -> std::optional<QualType> {
      Templates t = templates();
      auto args = t.deduce_call(pattern, shapes, {}, defaults);
      if (!args) return std::nullopt;
      if (pattern.constraints != ConstId::none) {
        auto params = ins().seqs.elements(pattern.template_params);
        auto constraint = t.substitute_constraint(pattern.constraints,
                                                   Bindings::of(params.empty() ? 0 : params[0].depth, *args));
        auto satisfied = constraint ? q_->satisfied(*constraint) : std::optional<bool>(false);
        if (satisfied && !*satisfied) return std::nullopt;
      }
      return t.specialization_type(pattern, *args);
    };
    bool nonmember_resolution = false;
    bool ambiguous_base = false;
    bool member_viable = false;
    if (found && found->member && found->member->is_function) {
      const bool rvalue = object.cat() != ValueCategory::lvalue;
      const QualType target = types().class_type(found->owner);
      const int depth = base_depth(lt, target);
      for (const MemberDeclRec* candidate : member_functions(found->owner, name)) {
        if (candidate->is_static) continue;
        QualType function_type = candidate->type;
        if (candidate->is_template) {
          auto pattern = q_->template_decl(candidate->entity);
          if (!pattern) continue;
          auto specialized = deduced_type(*pattern, {}, candidate->defaults);
          if (!specialized) continue;
          function_type = *specialized;
        }
        auto fn = types().rec(function_type);
        Ics rank = implicit_object_rank(fn, object_cv(lt), rvalue);
        if (!rank.viable || !fn.params.empty()) continue;
        member_viable = true;
        if (depth) {
          rank.rank = 2;  // derived-to-base conversion
          rank.base_depth = depth;
        }
        viable.push_back({rank, target, nullptr, candidate->is_template});
      }
    }
    for (const auto& candidate : set) {
      QualType function_type = candidate.type;
      const bool is_template = (candidate.flags & decl_flag::template_) != 0;
      if (is_template) {
        const std::array shapes{argument_shape(object)};
        auto specialized = deduced_type(candidate, shapes, found_defaults(candidate));
        if (!specialized) continue;
        function_type = *specialized;
      }
      auto fn = types().rec(function_type);
      if (fn.params.size() != 1) continue;
      const QualType parameter = fn.params.front();
      const QualType target = types().referee(parameter);
      // An exact match outranks a repeated-base conversion. Keep the diagnostic for
      // the remaining cases until repeated-base conversions participate in ranking.
      Ex binding_object = object;
      if (binding_object.cat() == ValueCategory::prvalue)
        binding_object.info.category = ValueCategory::xvalue;  // after temporary materialization
      if (types().is_class(target) && types().class_of(target) != types().class_of(lt) &&
          (!types().is_reference(parameter) ||
           (direct_reference_category(binding_object, types().rec(parameter)) &&
            cv_within(object_cv(lt), object_cv(target))))) {
        int paths = 0;
        base_path(types().class_of(lt), types().class_of(target), paths);
        if (paths > 1) {
          ambiguous_base = true;
          continue;
        }
      }
      const auto rank = ics(object, parameter);
      if (!rank.viable) continue;
      nonmember_resolution |= is_template || rank.rank >= Ics::user_defined;
      viable.push_back({rank, target, &candidate, is_template});
    }
    // The call below ranks non-member candidates alone, whatever their conversions and templates.
    const bool nonmembers_only = nonmember_resolution && !member_viable && !ambiguous_base;
    if (ambiguous_base && std::ranges::none_of(viable, [](const UnaryCandidate& c) { return c.rank.rank == 0; })) {
      diag(op_at, Diag::ref_qualified_operator_candidates);
      return Ex{NodeId::none, begin, end};
    }
    if (viable.empty()) return std::nullopt;
    if (!nonmembers_only) {
      auto better_conversion = [&](const UnaryCandidate& left, const UnaryCandidate& right) {
        if (left.rank.base_depth && right.rank.base_depth &&
            types().class_of(left.target) != types().class_of(right.target)) {
          if (base_depth(left.target, right.target)) return true;
          if (base_depth(right.target, left.target)) return false;
          return left.rank.ref && right.rank.ref && left.rank.to_rvalue && right.rank.to_rvalue &&
                 left.rank.rvalue_ref && !right.rank.rvalue_ref;
        }
        return left.rank < right.rank;
      };
      // [over.match.best.general]/2: then a non-template before a template specialization.
      auto better = [&](const UnaryCandidate& left, const UnaryCandidate& right) {
        return better_conversion(left, right) ||
               (!left.is_template && right.is_template && !better_conversion(right, left));
      };
      auto winner = viable.begin();
      for (auto candidate = std::next(winner); candidate != viable.end(); ++candidate)
        if (better(*candidate, *winner)) winner = candidate;
      auto undecided = [&](const UnaryCandidate& candidate) {
        return &candidate != &*winner && !better(*winner, candidate);
      };
      if (winner->is_template && std::ranges::any_of(viable, [&](const UnaryCandidate& candidate) {
            return candidate.is_template && undecided(candidate);
          })) {
        diag(op_at, Diag::ref_qualified_operator_candidates);  // partial ordering with a member template
        return Ex{NodeId::none, begin, end};
      }
      if (std::ranges::any_of(viable, undecided)) {
        diag(op_at, Diag::ambiguous_call, name);
        return Ex{NodeId::none, begin, end};
      }
      if (!winner->nonmember) {
        Ex access = member(object, false, name, object.end, op_at, op_at);
        Ex result = access.ok() ? call(access, {}, end, op_at) : access;
        return Ex{result.id, begin, end, result.info};
      }
      const auto selected = *winner->nonmember;
      set.assign(1, selected);
    }
  }
  const auto call_nonmember = [&]() {
    ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
    Ex callee = node(ast::DeclRef{info, set.front().entity}, begin, begin, info);
    overloads_[std::to_underlying(callee.id)] = std::move(set);
    return call(callee, std::move(operands), end, op_at, std::nullopt, enum_only);
  };
  if (member_only_failure) {
    if (set.empty()) return Ex{NodeId::none, begin, end};
    const OrdinaryOperatorChoice choice = ordinary_binary_choice(name, operands, set);
    if (choice.ambiguous) {
      truncate_diags(n);
      diag(op_at, Diag::ambiguous_call, name);
      return Ex{NodeId::none, begin, end};
    }
    if (!choice.nonmember) return Ex{NodeId::none, begin, end};
    truncate_diags(n);
    const ast::DeclSema selected = *choice.nonmember;
    set.assign(1, selected);
    const Ex result = call_nonmember();
    // A failure to use the selected function cannot revive a built-in candidate.
    return Ex{result.id, begin, end, result.info};
  }
  if (set.empty()) return ref_qualified_member;
  if (ref_qualified_member && (!binary || q_->dependent_call(op_at))) {
    const auto member_call = as<ast::MemberCall>(*ref_qualified_member);
    if (member_call) {
      const auto& member = w_->entities.rec(member_call->function);
      const auto fn = types().rec(member.type);
      const bool lvalue = operands[0].cat() == ValueCategory::lvalue;
      if (member.scope == types().class_of(lt) && fn.params.size() + 1 == operands.size() &&
          (binary ? fn.fn_ref == ast::RefQual::lvalue && lvalue : (fn.fn_ref == ast::RefQual::lvalue) == lvalue)) {
        const auto object = types().add_cv(types().class_type(member.scope), fn.fn_cv);
        std::vector<Ics> member_ranks{ics(operands[0], lvalue ? types().lvalue_ref(object) : types().rvalue_ref(object))};
        if (operands.size() == 2) member_ranks.push_back(ics(operands[1], fn.params[0]));
        // Keep the existing joint-ranking diagnostic unless the member dominates
        // every viable ordinary non-member candidate.
        if (std::ranges::all_of(member_ranks, &Ics::viable) &&
            std::ranges::all_of(set, [&](const ast::DeclSema& candidate) {
              if (candidate.flags & decl_flag::template_) return false;
              const auto free_fn = types().rec(candidate.type);
              if (free_fn.variadic) return false;
              if (free_fn.params.size() != operands.size()) return true;  // another arity of the operator
              std::vector<Ics> free_ranks;
              for (std::size_t i = 0; i < operands.size(); ++i) free_ranks.push_back(ics(operands[i], free_fn.params[i]));
              return !std::ranges::all_of(free_ranks, &Ics::viable) ||
                     better_conversions(member_ranks, free_ranks).value_or(false);
            }))
          return ref_qualified_member;
      }
    }
  }
  Ex r = call_nonmember();
  if (ref_qualified_member) {
    // Joint ranking is needed when the ref-qualified member competes with
    // at least one viable nonmember candidate.
    bool competing = r.ok();
    for (std::size_t i = n; !competing && i < diags_->size(); ++i)
      competing = (*diags_)[i].id == diag_id(Diag::deleted_function) ||
                  (*diags_)[i].id == diag_id(Diag::ambiguous_call) || (*diags_)[i].id == diag_id(Diag::not_constant);
    truncate_diags(n);
    if (!competing) return ref_qualified_member;
    diag(op_at, Diag::ref_qualified_operator_candidates);
    return Ex{NodeId::none, begin, end};
  }
  if (!r.ok()) {
    // A deleted or ambiguous choice makes the expression ill-formed ([over.match.oper]/3,
    // [over.match.best]), and so does a selected candidate whose argument conversion is an
    // immediate invocation that is not constant ([expr.const]); only when no candidate is
    // viable does the built-in operator apply.
    for (std::size_t i = n; i < diags_->size(); ++i)
      if ((*diags_)[i].id == diag_id(Diag::deleted_function) || (*diags_)[i].id == diag_id(Diag::ambiguous_call) ||
          (*diags_)[i].id == diag_id(Diag::not_constant))
        return Ex{NodeId::none, begin, end};
    truncate_diags(n);
    return std::nullopt;
  }
  return Ex{r.id, begin, end, r.info};
}

auto Sema::rewritten_equality(bool not_equal, Ex l, Ex r, u32 begin, u32 end, u32 op_at) -> std::optional<Ex> {
  if (!types().is_class(types().referee(l.type())) && !types().is_class(types().referee(r.type()))) return std::nullopt;
  auto selected = [&](const Ex& e) {
    if (auto c = as<ast::Call>(e)) return c->function;
    if (auto c = as<ast::MemberCall>(e)) return c->function;
    return EntityId::none;
  };
  // [over.match.oper]/4: a search for `operator!=` finds a corresponding declaration.
  auto suppressed = [&](EntityId f) {
    if (f == EntityId::none) return false;
    const EntityRec& fr = w_->entities.rec(f);
    const TypeRec ft = types().rec(fr.type);
    const NameId ne = ins().names.intern("operator!=");
    auto corresponds = [&](QualType t) {
      const TypeRec g = types().rec(t);
      if (g.params.size() != ft.params.size() || g.fn_cv != ft.fn_cv || g.fn_ref != ft.fn_ref) return false;
      for (std::size_t i = 0; i < g.params.size(); ++i)
        if (types().canonical(g.params[i]) != types().canonical(ft.params[i])) return false;
      return true;
    };
    // A global namespace member's scope is none.
    if (const ClassRec* c = fr.scope == EntityId::none ? nullptr : class_rec(fr.scope))
      return std::ranges::any_of(c->members, [&](const MemberDeclRec& m) {
        return m.name == ne && m.is_function && !m.is_template && corresponds(m.type);
      });
    return std::ranges::any_of(q_->namespace_members(fr.scope, ne), [&](const ast::DeclSema& d) {
      return d.kind == ast::DeclKind::function && !(d.flags & decl_flag::template_) && corresponds(d.type);
    });
  };
  const std::size_t n = diag_count();
  std::optional<Ex> equal;
  if (not_equal) equal = overloaded("==", {l, r}, begin, end, op_at);
  if (equal && equal->ok() && suppressed(selected(*equal))) equal.reset();
  if (!equal) {
    truncate_diags(n);
    equal = overloaded("==", {r, l}, begin, end, op_at);
    if (equal && equal->ok() && suppressed(selected(*equal))) equal.reset();
  }
  if (!equal) {
    truncate_diags(n);
    return std::nullopt;
  }
  if (!equal->ok()) return Ex{NodeId::none, begin, end};
  // [over.match.oper]: the selected function's return type, not the call's, is cv `bool`.
  const EntityId f = selected(*equal);
  const QualType result = f != EntityId::none ? types().rec(w_->entities.rec(f).type).inner : equal->type();
  if (types().is_reference(result) || Types::unqualified(types().canonical(result)) != builtin(Builtin::bool_)) {
    diag(op_at, Diag::rewritten_equality_return, result);
    return Ex{NodeId::none, begin, end};
  }
  if (!not_equal) return Ex{equal->id, begin, end, equal->info};
  return unary(ast::UnaryOp::not_, Ex{equal->id, begin, end, equal->info}, begin, end, op_at);
}

auto Sema::unary(ast::UnaryOp op, Ex e, u32 begin, u32 end, u32 op_at) -> Ex {
  using enum ast::UnaryOp;
  if (!e.ok()) return {NodeId::none, begin, end};
  if (dependent(e)) {
    const bool type_dependent = has_proven_dependent_type(e);
    if (type_dependent) dependent_calls.push_back(op_at);
    if (op == not_) {
      const u8 flags = expr_flag::dependent | (e.info.flags & expr_flag::deferred_decay_copy);
      ExprInfo info{builtin(Builtin::dependent), ValueCategory::prvalue, flags};
      return node(ast::Unary{info, op, at(e, begin)}, begin, end, info);
    }
    // [temp.dep.expr]: for a genuinely type-dependent operand, even selecting the operator can
    // require replay. The result need not have the operand's type.
    const u8 flags = type_dependent ? expr_flag::type_dependent : 0;
    return dependent_node({e}, begin, end, std::nullopt, ValueCategory::prvalue, flags);
  }
  static constexpr std::string_view unary_spellings[] = {"+", "-", "!", "~", "*", "&", "++", "--", "", ""};
  if (std::string_view sp = unary_spellings[std::to_underlying(op)]; !sp.empty())
    if (auto o = overloaded(sp, {e}, begin, end, op_at)) return *o;
  // `+closure`: the built-in `+` of the pointer its conversion function gives ([over.built]/9).
  if (op == plus && types().is_class(e.type()))
    if (const ClassRec* r = class_rec(types().class_of(e.type())); r && r->closure)
      for (const MemberDeclRec& m : r->members)
        if (m.is_static && !m.is_template && (m.name == ins().names.intern("__invoke") ||
                                              m.name == ins().names.intern("operator()"))) {
          Ex p = invoker_pointer(e, {m.entity, m.type});
          return {p.id, begin, end, p.info};
        }
  auto make = [&](Ex operand, QualType t, ValueCategory cat) {
    ExprInfo info{t, cat, 0, cat == ValueCategory::prvalue ? u8(0) : operand.info.bit_width};
    return node(ast::Unary{info, op, at(operand, begin)}, begin, end, info);
  };
  switch (op) {
  case plus:
  case minus:
  case compl_: {
    Ex o = promote(e);
    QualType t = o.type();
    bool ok = op == compl_ ? types().is_integral(t) : types().is_arithmetic(t) || (op == plus && types().is_pointer(t));
    if (!ok) {
      diag(op_at, Diag::invalid_operand, e.type());
      return {NodeId::none, begin, end};
    }
    return make(o, t, ValueCategory::prvalue);
  }
  case not_: {
    Ex o = to_bool(e, e.begin);
    if (!o.ok()) return {NodeId::none, begin, end};
    return make(o, builtin(Builtin::bool_), ValueCategory::prvalue);
  }
  case deref: {
    Ex o = rvalue(e);
    if (!types().is_pointer(o.type()) || types().is_void(types().pointee(o.type()))) {
      diag(op_at, Diag::indirection_not_pointer, e.type());
      return {NodeId::none, begin, end};
    }
    return make(o, types().pointee(o.type()), ValueCategory::lvalue);
  }
  case address_of:
    return builtin_address_of(e, begin, end, op_at);
  case pre_inc:
  case pre_dec:
  case post_inc:
  case post_dec: {
    // [expr.pre.incr], [expr.post.incr]: a modifiable lvalue of arithmetic type other than
    // bool, or a pointer to a complete object type.
    if (!check_modifiable(e, e.begin)) return {NodeId::none, begin, end};
    QualType t = e.type();
    if (types().is(t, Builtin::bool_)) {
      diag(op_at, Diag::increment_bool);
      return {NodeId::none, begin, end};
    }
    if (!types().is_arithmetic(t) && !is_object_pointer(t)) {
      diag(op_at, Diag::invalid_operand, t);
      return {NodeId::none, begin, end};
    }
    if (types().is_pointer(t) && !require_supported_virtual_layout(types().pointee(t), op_at))
      return {NodeId::none, begin, end};
    bool pre = op == pre_inc || op == pre_dec;
    return make(e, pre ? t : Types::unqualified(t), pre ? ValueCategory::lvalue : ValueCategory::prvalue);
  }
  }
  std::unreachable();
}

auto Sema::member_pointer_access(Ex object, Ex pointer, bool arrow, u32 op_at) -> Ex {
  u32 begin = object.begin, end = pointer.end;
  Ex fail{NodeId::none, begin, end};
  pointer = rvalue(pointer);
  if (!pointer.ok()) return fail;
  TypeRec member = types().rec(pointer.type());
  if (member.kind != TypeKind::member_pointer) {
    diag(op_at, Diag::member_pointer_operand, arrow ? lex::Tk::arrowstar : lex::Tk::periodstar, pointer.type());
    return fail;
  }
  QualType owner = object.type();
  if (arrow) {
    object = rvalue(object);
    if (!object.ok()) return fail;
    if (!types().is_pointer(object.type()) || !types().is_class(types().pointee(object.type()))) {
      diag(op_at, Diag::member_pointer_arrow_object, object.type());
      return fail;
    }
    owner = types().pointee(object.type());
  } else if (!types().is_class(owner)) {
    diag(op_at, Diag::member_pointer_object, owner);
    return fail;
  }
  if (!same_unqualified(owner, member.member_owner)) {
    int paths = 0;
    if (base_path(types().class_of(owner), types().class_of(member.member_owner), paths))
      diag(op_at, Diag::member_pointer_values);
    else
      diag(op_at, Diag::member_pointer_wrong_class, member.member_owner, owner);
    return fail;
  }
  if (types().is_function(member.inner)) {
    diag(op_at, Diag::member_pointer_values);
    return fail;
  }
  if (!arrow && !object.glvalue()) object = materialize(object, owner, false);
  ValueCategory cat = arrow || object.cat() == ValueCategory::lvalue ? ValueCategory::lvalue : ValueCategory::xvalue;
  ExprInfo info{types().add_cv(member.inner, object_cv(owner)), cat, 0};
  const auto op = arrow ? ast::OrderedOp::arrow_star : ast::OrderedOp::dot_star;
  return node(ast::OrderedBinary{info, op, at(object, begin), at(pointer, begin)}, begin, end, info);
}

auto Sema::binary(BinOp op, Ex l, Ex r, u32 op_at) -> Ex {
  using enum BinOp;
  u32 begin = l.begin, end = r.end;
  if (!l.ok() || !r.ok()) return {NodeId::none, begin, end};
  if (dependent(l) || dependent(r)) {
    if (has_proven_dependent_type(l) || has_proven_dependent_type(r)) {
      dependent_calls.push_back(op_at);
      return dependent_node({l, r}, begin, end, std::nullopt, ValueCategory::prvalue, expr_flag::type_dependent);
    }
    if (types().is_integral(l.type()) && types().is_integral(r.type())) {
      switch (op) {
      case add: case sub: case mul: case div: case rem:
      case bit_and: case bit_xor: case bit_or: {
        auto replay_operand = [&](Ex e) {
          if (!dependent(e) || (e.info.flags & expr_flag::dependent_integral_arithmetic)) return true;
          while (auto p = as<ast::Paren>(e)) e.id = p->operand.id;
          auto literal = as<ast::Literal>(e);
          return literal && ins().values.is<ast::VDepParam>(literal->value);
        };
        const u8 flags = replay_operand(l) && replay_operand(r) ? expr_flag::dependent_integral_arithmetic : u8{0};
        return dependent_node({l, r}, begin, end, integral_common_type(l, r), ValueCategory::prvalue, flags);
      }
      default: break;
      }
    }
    return dependent_node({l, r}, begin, end);
  }
  if (op == dot_star || op == arrow_star) return member_pointer_access(l, r, op == arrow_star, op_at);
  const bool equality_rewrite = op == eq || op == ne;
  const bool three_way_rewrite = op == spaceship || op == lt || op == gt || op == le || op == ge;
  if (three_way_rewrite && !rewriting_three_way_)
    if (auto result = ranked_three_way(op, l, r, begin, end, op_at)) return *result;
  if (auto o = overloaded(binary_spellings[std::to_underlying(op)], {l, r}, begin, end, op_at,
                          /*rank_failed_member=*/!equality_rewrite && !three_way_rewrite))
    return *o;
  if (equality_rewrite)
    if (auto o = rewritten_equality(op == ne, l, r, begin, end, op_at)) return *o;
  if (three_way_rewrite)
    if (auto o = rewritten_three_way(op, l, r, begin, end, op_at)) return *o;
  if (op == spaceship) return builtin_three_way(l, r, begin, end, op_at);
  auto bin = [&](ast::BinaryOp k, Ex a, Ex c, QualType t) {
    ExprInfo info{t, ValueCategory::prvalue, 0};
    return node(ast::Binary{info, k, at(a, begin), at(c, begin)}, begin, end, info);
  };
  auto ordered = [&](ast::OrderedOp k, Ex a, Ex c, ExprInfo info) {
    return node(ast::OrderedBinary{info, k, at(a, begin), at(c, begin)}, begin, end, info);
  };
  auto invalid = [&] {
    diag(op_at, Diag::invalid_operands, l.type(), r.type());
    return Ex{NodeId::none, begin, end};
  };
  switch (op) {
  case logical_and:
  case logical_or: {
    Ex a = to_bool(l, l.begin), c = to_bool(r, r.begin);
    if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
    return ordered(op == logical_and ? ast::OrderedOp::logical_and : ast::OrderedOp::logical_or, a, c,
                   {builtin(Builtin::bool_), ValueCategory::prvalue, 0});
  }
  case comma:
    // [expr.comma]: the left operand is discarded. Only the right produces the result.
    if (!l.glvalue() && (!require_concrete_call_result(l.type(), l.begin) || !check_destructor_access(l.type(), l.begin)))
      return {NodeId::none, begin, end};
    return ordered(ast::OrderedOp::comma, discard_gcc_reinterpretation(l), r, r.info);
  case shl:
  case shr: {
    Ex a = promote(l), c = promote(r);
    if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
    if (!types().is_integral(a.type()) || !types().is_integral(c.type())) return invalid();
    return ordered(op == shl ? ast::OrderedOp::shl : ast::OrderedOp::shr, a, c, {a.type(), ValueCategory::prvalue, 0});
  }
  default: break;
  }
  Ex a = rvalue(l), c = rvalue(r);
  if (!a.ok() || !c.ok()) return {NodeId::none, begin, end};
  // [expr.arith.conv]: unscoped enumerations are promoted; a scoped one is compared only with
  // its own type, by value; enumerations do not otherwise mix.
  if (types().is_enum(a.type()) || types().is_enum(c.type())) {
    bool cmp = op == lt || op == gt || op == le || op == ge || op == eq || op == ne;
    if (is_scoped_enum(a.type()) || is_scoped_enum(c.type())) {
      if (!cmp || types().canonical(Types::unqualified(a.type())) != types().canonical(Types::unqualified(c.type())))
        return invalid();
      QualType u = underlying_of(a.type());
      a = convert_node(a, ast::ConversionKind::integral, u);
      c = convert_node(c, ast::ConversionKind::integral, u);
    } else {
      if (types().is_enum(a.type()) && types().is_enum(c.type()) &&
          types().canonical(Types::unqualified(a.type())) != types().canonical(Types::unqualified(c.type())))
        return invalid();  // P2864: arithmetic between different enumerations is ill-formed
      if (types().is_floating(a.type()) || types().is_floating(c.type()))
        return invalid();  // [expr.arith.conv]/1: and between an enumeration and a floating type
      if (types().is_enum(a.type())) a = promote(a);
      if (types().is_enum(c.type())) c = promote(c);
    }
  }
  QualType ta = a.type(), tc = c.type();
  bool arith = types().is_arithmetic(ta) && types().is_arithmetic(tc);
  bool integral = types().is_integral(ta) && types().is_integral(tc);
  auto arithmetic_bin = [&](ast::BinaryOp kind) {
    QualType t = arithmetic_conversions(a, c);
    return bin(kind, a, c, t);
  };
  static constexpr std::array<ast::BinaryOp, 19> kinds{
      ast::BinaryOp::mul, ast::BinaryOp::div, ast::BinaryOp::rem, ast::BinaryOp::add, ast::BinaryOp::sub,
      ast::BinaryOp::mul, ast::BinaryOp::mul, ast::BinaryOp::lt, ast::BinaryOp::gt, ast::BinaryOp::le,
      ast::BinaryOp::ge, ast::BinaryOp::eq, ast::BinaryOp::ne, ast::BinaryOp::bit_and, ast::BinaryOp::bit_xor,
      ast::BinaryOp::bit_or, ast::BinaryOp::mul, ast::BinaryOp::mul, ast::BinaryOp::mul};
  ast::BinaryOp k = kinds[std::to_underlying(op)];
  switch (op) {
  case mul:
  case div:
    if (!arith) return invalid();
    return arithmetic_bin(k);
  case rem:
  case bit_and:
  case bit_xor:
  case bit_or:
    if (!integral) return invalid();
    return arithmetic_bin(k);
  case add:
  case sub:
    if (arith) return arithmetic_bin(k);
    // [expr.add]: a pointer to a complete object type and an integer; or, for `-`, two
    // pointers to the same type, giving std::ptrdiff_t (long).
    if (is_object_pointer(ta) && types().is_integral(tc)) {
      if (!require_supported_virtual_layout(types().pointee(ta), op_at)) return {NodeId::none, begin, end};
      return bin(k, a, promote(c), ta);
    }
    if (op == add && types().is_integral(ta) && is_object_pointer(tc)) {
      if (!require_supported_virtual_layout(types().pointee(tc), op_at)) return {NodeId::none, begin, end};
      return bin(k, promote(a), c, tc);
    }
    if (op == sub && is_object_pointer(ta) && is_object_pointer(tc) &&
        Types::unqualified(types().pointee(ta)) == Types::unqualified(types().pointee(tc))) {
      if (!require_supported_virtual_layout(types().pointee(ta), op_at)) return {NodeId::none, begin, end};
      return bin(k, a, c, builtin(Builtin::long_));
    }
    if (types().is_pointer(ta) && types().is_integral(tc)) {
      diag(op_at, Diag::pointer_arithmetic_void, ta);
      return {NodeId::none, begin, end};
    }
    return invalid();
  case lt:
  case gt:
  case le:
  case ge:
  case eq:
  case ne: {
    QualType b = builtin(Builtin::bool_);
    if (arith) {
      arithmetic_conversions(a, c);
      return bin(k, a, c, b);
    }
    // [expr.rel], [expr.eq]: pointers converted to their composite pointer type; for the
    // equality operators also a pointer and a null pointer constant.
    bool equality = op == eq || op == ne;
    if (equality && ((types().is_member_pointer(ta) && (same_unqualified(ta, tc) || is_null_pointer_constant(r))) ||
                     (types().is_member_pointer(tc) && is_null_pointer_constant(l)))) {
      diag(op_at, Diag::member_pointer_values);
      return {NodeId::none, begin, end};
    }
    switch (composite_pointers(a, c, /*function_pointers=*/true, op_at)) {
    case Composite::converted: return bin(k, a, c, b);
    case Composite::failed: return {NodeId::none, begin, end};
    case Composite::none: break;
    }
    if (equality && types().is(ta, Builtin::nullptr_t) && types().is(tc, Builtin::nullptr_t)) return bin(k, a, c, b);
    if (equality && types().is_pointer(ta) && (types().is(tc, Builtin::nullptr_t) || is_null_pointer_constant(r)))
      return bin(k, a, convert_node(c, ast::ConversionKind::null_pointer, ta), b);
    if (equality && types().is_pointer(tc) && (types().is(ta, Builtin::nullptr_t) || is_null_pointer_constant(l)))
      return bin(k, convert_node(a, ast::ConversionKind::null_pointer, tc), c, b);
    return invalid();
  }
  default: break;
  }
  std::unreachable();
}

auto Sema::composite_pointers(Ex& a, Ex& c, bool function_pointers, u32 op_at) -> Composite {
  QualType ta = a.type(), tc = c.type();
  if (auto to = composite_void_pointer(ta, tc)) {
    a = convert(a, *to, op_at);
    c = convert(c, *to, op_at);
    return a.ok() && c.ok() ? Composite::converted : Composite::failed;
  }
  if (auto to = composite_function_pointer(ta, tc)) {
    if (!function_pointers) return Composite::none;
    a = convert(a, *to, op_at);
    c = convert(c, *to, op_at);
    return Composite::converted;
  }
  if (!types().is_pointer(ta) || !types().is_pointer(tc)) return Composite::none;
  QualType pa = types().pointee(ta), pc = types().pointee(tc);
  if (!function_pointers && (types().is_function(pa) || types().is_function(pc))) return Composite::none;
  // [expr.type]/4.6: a pointer to a derived class converted to one to its base class.
  if (base_depth(pa, pc)) {
    a = to_base(a, types().class_of(pc), op_at);
    if (!a.ok()) return Composite::failed;
    ta = a.type();
    pa = types().pointee(ta);
  } else if (base_depth(pc, pa)) {
    c = to_base(c, types().class_of(pa), op_at);
    if (!c.ok()) return Composite::failed;
    tc = c.type();
    pc = types().pointee(tc);
  }
  QualType unqualified = Types::unqualified(pa);
  Cv ca = Types::cv(pa), cc = Types::cv(pc);
  if (unqualified != Types::unqualified(pc)) {
    // [expr.type], [conv.qual]: complete arrays of the same shape combine the
    // cv-qualifiers of their elements, including through nested array dimensions.
    if (!types().is_array(pa)) return Composite::none;
    unqualified = object_unqualified(pa);
    if (unqualified != object_unqualified(pc) || !is_complete(pa)) return Composite::none;
    ca = object_cv(pa);
    cc = object_cv(pc);
  }
  Cv cv = Cv(std::to_underlying(ca) | std::to_underlying(cc));
  QualType composite = types().pointer(types().add_cv(unqualified, cv));
  if (ta != composite) a = convert_node(a, ast::ConversionKind::qualification, composite);
  if (tc != composite) c = convert_node(c, ast::ConversionKind::qualification, composite);
  return Composite::converted;
}

auto Sema::comparison_category(Ordering k, std::optional<u32> at_) -> std::optional<ComparisonCategory> {
  static constexpr std::string_view names[] = {"partial_ordering", "weak_ordering", "strong_ordering"};
  const NameId name = ins().names.intern(names[std::to_underlying(k)]);
  auto missing = [&] -> std::optional<ComparisonCategory> {
    if (at_) diag(*at_, Diag::comparison_category_missing, name);
    return std::nullopt;
  };
  const auto std_scope = scope_member(global_scope(), ins().names.intern("std"), false);
  if (!std_scope || !std_scope->ns) return missing();
  const auto found = scope_member(*std_scope, name, false);
  if (!found || !found->type || !types().is_class(*found->type) || !is_complete(*found->type)) return missing();
  const QualType type = Types::unqualified(*found->type);
  const EntityId cls = types().class_of(type);
  std::vector<std::string_view> members{"less", k == Ordering::strong ? "equal" : "equivalent", "greater"};
  if (k == Ordering::partial) members.push_back("unordered");
  ComparisonCategory r{type, {}};
  for (std::string_view m : members) {
    const auto value = find_member(cls, ins().names.intern(m));
    if (!value || value->ambiguous || !value->member || value->member->is_function || !value->member->is_static ||
        Types::unqualified(types().canonical(value->member->type)) != types().canonical(type))
      return missing();
    if (at_ && !q_->use_static_member(value->member->entity)) return std::nullopt;
    r.values.push_back(value->member->entity);
  }
  return r;
}

auto Sema::builtin_three_way(Ex l, Ex r, u32 begin, u32 end, u32 op_at) -> Ex {
  const Ex fail{NodeId::none, begin, end};
  auto invalid = [&] {
    diag(op_at, Diag::invalid_operands, l.type(), r.type());
    return fail;
  };
  // /6 converts an array operand only when the other is an object pointer.
  if (types().is_array(types().referee(l.type())) && types().is_array(types().referee(r.type()))) return invalid();
  Ex a = rvalue(l), c = rvalue(r);
  if (!a.ok() || !c.ok()) return fail;
  // /3: a `bool` operand only with another.
  if (types().is(Types::unqualified(a.type()), Builtin::bool_) != types().is(Types::unqualified(c.type()), Builtin::bool_))
    return invalid();
  std::optional<Ordering> ordering;
  // The operands before the usual arithmetic conversions, for the narrowing check. A bit-field
  // keeps its width, which can make the conversion not narrowing ([dcl.init.list]/7.4).
  Ex from_a = a, from_c = c;
  from_a.info.bit_width = l.info.bit_width;
  from_c.info.bit_width = r.info.bit_width;
  if (types().is_enum(a.type()) || types().is_enum(c.type())) {
    if (types().canonical(Types::unqualified(a.type())) == types().canonical(Types::unqualified(c.type()))) {
      // /5: two of one enumeration, compared as its underlying type.
      const QualType u = underlying_of(a.type());
      a = convert_node(a, ast::ConversionKind::integral, u);
      c = convert_node(c, ast::ConversionKind::integral, u);
      ordering = Ordering::strong;
    } else {
      // /4: an unscoped enumeration with an integral type, as its promoted type.
      auto integral = [&](const Ex& e) { return types().is_integral(e.type()) || is_unscoped_enum(e.type()); };
      if (is_scoped_enum(a.type()) || is_scoped_enum(c.type()) ||
          (types().is_enum(a.type()) && types().is_enum(c.type())) || !integral(a) || !integral(c))
        return invalid();
      if (types().is_enum(a.type())) a = promote(a);
      if (types().is_enum(c.type())) c = promote(c);
    }
  }
  if (!ordering && types().is_arithmetic(a.type()) && types().is_arithmetic(c.type())) {
    // /4: the usual arithmetic conversions, where a narrowing conversion other than from an
    // integral type to a floating type is ill-formed.
    const QualType t = arithmetic_conversions(a, c);
    for (const Ex& from : {from_a, from_c}) {
      if (types().is_floating(t) && !types().is_floating(from.type())) continue;
      if (narrows(from, t)) {
        diag(from.begin, Diag::three_way_narrowing, from.type(), t);
        return fail;
      }
    }
    ordering = types().is_floating(t) ? Ordering::partial : Ordering::strong;
  }
  if (!ordering && types().is_pointer(a.type()) && types().is_pointer(c.type())) {
    // /6: object pointers converted to their composite pointer type.
    switch (composite_pointers(a, c, /*function_pointers=*/false, op_at)) {
    case Composite::converted: ordering = Ordering::strong; break;
    case Composite::failed: return fail;
    case Composite::none: break;
    }
  }
  if (!ordering) return invalid();  // /7
  const auto category = comparison_category(*ordering, op_at);
  if (!category) return fail;
  ExprInfo info{category->type, ValueCategory::prvalue, 0};
  const auto results = ins().intern_seq(std::span<const EntityId>(category->values));
  return node(ast::ThreeWay{info, at(a, begin), at(c, begin), results}, begin, end, info);
}

auto Sema::ranked_three_way(BinOp op, Ex l, Ex r, u32 begin, u32 end, u32 op_at) -> std::optional<Ex> {
  const std::array operand_types{types().referee(l.type()), types().referee(r.type())};
  if (!types().is_class(operand_types[0]) && !types().is_class(operand_types[1])) return std::nullopt;
  const std::array operands{l, r};
  const NameId conversion = ins().names.intern(lex::conversion_function_name);
  std::array<EntityId, 2> owners{EntityId::none, EntityId::none};
  for (std::size_t i = 0; i < operands.size(); ++i) {
    const Ex& operand = operands[i];
    const QualType type = operand_types[i];
    if (operand.info.bit_width || Types::is_volatile(types().canonical(type))) return std::nullopt;
    if (types().is_integral(type)) continue;
    if (!types().is_class(type)) return std::nullopt;
    const EntityId owner = types().class_of(type);
    const auto& entity = w_->entities.rec(owner);
    if (entity.kind != EntityKind::class_ || entity.templ != EntityId::none) return std::nullopt;
    const ClassRec* record = class_rec(owner);
    if (!record || !record->member_declarations_valid || !record->bases.empty() ||
        std::ranges::contains(record->members, conversion, &MemberDeclRec::name) || !is_complete(type))
      return std::nullopt;
    owners[i] = owner;
  }

  const bool instantiated = q_->dependent_call(op_at);  // [temp.dep.candidate]
  // The complete ordinary relational candidate set is a separate ranking case.
  // Check declarations, including those for which a call would fail.
  if (op != BinOp::spaceship) {
    const NameId ordinary = ins().names.intern(std::string("operator") +
                                              std::string(binary_spellings[std::to_underlying(op)]));
    if (owners[0] != EntityId::none && find_member(owners[0], ordinary)) return std::nullopt;
    auto nonmembers = ordinary_operator_nonmembers(ordinary);
    add_operator_adl(nonmembers, ordinary, operands, /*at_instantiation=*/false);
    if (instantiated) add_operator_adl(nonmembers, ordinary, operands, /*at_instantiation=*/true);
    if (!nonmembers.empty()) return std::nullopt;
  }

  auto argument_rank = [&](const Ex& operand, QualType parameter) -> std::optional<Ics> {
    const QualType source = types().referee(operand.type());
    const QualType target = types().referee(parameter);
    if (Types::is_volatile(types().canonical(target))) return std::nullopt;
    if (same_unqualified(source, target)) return ics(operand, parameter);
    if (types().is_class(target)) {
      const ClassRec* record = class_rec(types().class_of(target));
      if (!record || !record->member_declarations_valid || !is_complete(target) || has_user_ctors(target))
        return std::nullopt;
      return Ics{};
    }
    // The source class has no bases or conversion functions.
    if (types().is_class(source)) return Ics{};
    if (!types().is_integral(target) || types().is_reference(parameter)) return std::nullopt;
    return ics(operand, parameter);
  };
  struct Ranked {
    std::array<Ics, 2> conversions{};
    const ast::DeclSema* nonmember = nullptr;
    const MemberDeclRec* member = nullptr;
    bool reversed = false;
  };
  const NameId name = ins().names.intern("operator<=>");
  auto nonmembers = ordinary_operator_nonmembers(name);
  add_operator_adl(nonmembers, name, operands, /*at_instantiation=*/false);
  if (instantiated) add_operator_adl(nonmembers, name, operands, /*at_instantiation=*/true);
  std::vector<Ranked> viable;
  for (std::size_t first = 0; first < operands.size(); ++first) {
    const std::size_t second = 1 - first;
    const bool reversed = first != 0;
    for (const ast::DeclSema& nonmember : nonmembers) {
      const auto function = types().rec(nonmember.type);
      if ((nonmember.flags & decl_flag::template_) || function.variadic || function.params.size() != 2)
        return std::nullopt;
      const auto first_rank = argument_rank(operands[first], function.params[0]);
      if (first_rank && !first_rank->viable) continue;
      const auto second_rank = argument_rank(operands[second], function.params[1]);
      if (second_rank && !second_rank->viable) continue;
      if (!first_rank || !second_rank) return std::nullopt;
      Ranked candidate{.nonmember = &nonmember, .reversed = reversed};
      candidate.conversions[first] = *first_rank;
      candidate.conversions[second] = *second_rank;
      viable.push_back(candidate);
    }
    if (owners[first] == EntityId::none) continue;
    for (const auto* member : member_functions(owners[first], name)) {
      const auto function = types().rec(member->type);
      if (member->is_static || member->is_template || function.variadic || function.params.size() != 1 ||
          (function.fn_cv != Cv::none && function.fn_cv != Cv::const_))
        return std::nullopt;
      const Ex& receiver = operands[first];
      const auto object = implicit_object_rank(function, object_cv(receiver.type()),
                                               receiver.cat() != ValueCategory::lvalue);
      if (!object.viable) continue;
      const auto argument = argument_rank(operands[second], function.params[0]);
      if (!argument) return std::nullopt;
      if (!argument->viable) continue;
      Ranked candidate{.member = member, .reversed = reversed};
      candidate.conversions[first] = object;
      candidate.conversions[second] = *argument;
      viable.push_back(candidate);
    }
  }
  if (viable.empty() || viable.front().reversed || !viable.back().reversed) return std::nullopt;
  const auto better = [&](const Ranked& a, const Ranked& b) {
    if (auto rank = better_conversions(a.conversions, b.conversions)) return *rank;
    // For <=> the forward candidate is non-rewritten. For a relational operator
    // both are rewritten and the forward candidate is not synthesized.
    return !a.reversed && b.reversed;
  };
  const auto selected = std::ranges::find_if(viable, [&](const Ranked& candidate) {
    return std::ranges::all_of(viable, [&](const Ranked& other) {
      return &candidate == &other || better(candidate, other);
    });
  });
  if (selected == viable.end()) {
    diag(op_at, Diag::ambiguous_call, name);
    return Ex{NodeId::none, begin, end};
  }

  const Ex first = selected->reversed ? r : l;
  const Ex second = selected->reversed ? l : r;
  Ex compared{NodeId::none, begin, end};
  if (selected->member) {
    Ex callee = member(first, false, name, first.end, op_at, op_at);
    if (!callee.ok()) return Ex{NodeId::none, begin, end};
    bound_.at(std::to_underlying(callee.id)).cands.assign(1, selected->member);
    compared = call(callee, {second}, end, op_at);
  } else {
    ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
    Ex callee = node(ast::DeclRef{info, selected->nonmember->entity}, begin, begin, info);
    overloads_[std::to_underlying(callee.id)] = {*selected->nonmember};
    compared = call(callee, {first, second}, end, op_at);
  }
  if (!compared.ok()) return Ex{NodeId::none, begin, end};
  if (op == BinOp::spaceship && !selected->reversed) return Ex{compared.id, begin, end, compared.info};
  return apply_three_way_rewrite(op, compared, selected->reversed, begin, end, op_at);
}

auto Sema::assign(AsgOp op, Ex l, Ex r, u32 op_at) -> Ex {
  u32 begin = l.begin, end = r.end;
  if (!l.ok() || !r.ok()) return {NodeId::none, begin, end};
  if (dependent(l) || dependent(r)) {
    if (op == AsgOp::assign || (!has_proven_dependent_type(l) && !has_proven_dependent_type(r)))
      return dependent_node({l, r}, begin, end, std::nullopt, ValueCategory::lvalue);
    dependent_calls.push_back(op_at);
    return dependent_node({l, r}, begin, end, std::nullopt, ValueCategory::lvalue, expr_flag::type_dependent);
  }
  if (op == AsgOp::assign && types().is_class(l.type())) {
    NameId name = ins().names.intern("operator=");
    Ex callee = member(l, false, name, l.end, op_at, op_at);
    return callee.ok() ? member_call(callee, {r}, end, op_at, true) : Ex{NodeId::none, begin, end};
  }
  // `=` is a member only ([over.assign]); `operator_function_ok` rejects non-member ones.
  static constexpr std::string_view assign_spellings[] = {"=", "*=", "/=", "%=", "+=", "-=", "<<=", ">>=", "&=", "^=", "|="};
  if (auto o = overloaded(assign_spellings[std::to_underlying(op)], {l, r}, begin, end, op_at)) return *o;
  if (!check_modifiable(l, op_at)) return {NodeId::none, begin, end};
  QualType lt = l.type();
  QualType computation = Types::unqualified(lt);
  Ex c = r;
  if (op == AsgOp::assign) {
    c = convert(r, lt, r.begin);
  } else {
    Ex lv = rvalue(l);  // only to read its type; the IR loads the left operand itself
    QualType tl = lv.type();
    // [expr.arith.conv]/1: an enumeration and a floating type do not mix.
    if ((types().is_enum(tl) && types().is_floating(r.type())) || (types().is_floating(tl) && types().is_enum(r.type()))) {
      diag(op_at, Diag::invalid_operands, lt, r.type());
      return {NodeId::none, begin, end};
    }
    Ex rv = is_unscoped_enum(r.type()) ? promote(r) : rvalue(r);  // [conv.prom]
    if (!rv.ok()) return {NodeId::none, begin, end};
    QualType tr = rv.type();
    bool shift = op == AsgOp::shl || op == AsgOp::shr;
    bool integral_only = shift || op == AsgOp::rem || op == AsgOp::bit_and || op == AsgOp::bit_xor || op == AsgOp::bit_or;
    if (types().is_arithmetic(tl) && types().is_arithmetic(tr) &&
        (!integral_only || (types().is_integral(tl) && types().is_integral(tr)))) {
      if (shift) {
        computation = builtin(sema::promoted(w_->target, *types().builtin_of(tl), lv.info.bit_width));
        c = promote(rv);
      } else {
        Builtin left = sema::promoted(w_->target, *types().builtin_of(tl), lv.info.bit_width);
        Builtin right = sema::promoted(w_->target, *types().builtin_of(tr), rv.info.bit_width);
        Builtin common = common_integer(w_->target, left, right);
        computation = builtin(common);
        c = convert(rv, computation, r.begin);
      }
    } else if ((op == AsgOp::add || op == AsgOp::sub) && is_object_pointer(tl) && types().is_integral(tr)) {
      if (!require_supported_virtual_layout(types().pointee(tl), op_at)) return {NodeId::none, begin, end};
      computation = tl;
      c = promote(rv);
    } else {
      diag(op_at, Diag::invalid_operands, lt, r.type());
      return {NodeId::none, begin, end};
    }
  }
  if (!c.ok()) return {NodeId::none, begin, end};
  ExprInfo info{lt, ValueCategory::lvalue, 0, l.info.bit_width};
  return node(ast::Assign{info, ast::AssignOp(std::to_underlying(op)), at(l, begin), at(c, begin), computation}, begin,
              end, info);
}

auto Sema::assign_list(Ex l, const Braced& b, u32 op_at) -> Ex {
  u32 begin = l.begin, end = b.end;
  if (l.ok() && (dependent(l) || dependent(b))) return dependent_node({l}, begin, end, std::nullopt, ValueCategory::lvalue);
  if (!l.ok() || !check_modifiable(l, op_at)) return {NodeId::none, begin, end};
  const bool class_copy = types().is_class(l.type());
  if (class_copy) {
    if (!require_complete(l.type(), op_at) || !require_supported_virtual_layout(l.type(), op_at))
      return {NodeId::none, begin, end};
    const auto* cls = class_rec(types().class_of(l.type()));
    const auto* copy = cls ? cls->implicit_copy_assignment() : nullptr;
    CopyAssignment state = CopyAssignment::unsupported;
    if (copy && std::ranges::count(cls->members, copy->name, &MemberDeclRec::name) == 1)
      state = copy->copy_assignment == CopyAssignment::memberwise ? CopyAssignment::unsupported
                                                                  : copy->copy_assignment;
    if (!usable_copy_assignment(state, Types::unqualified(l.type()), op_at)) return {NodeId::none, begin, end};
    const auto* source = class_reference_list_operand(l.type(), b);
    if ((!source || !source->glvalue()) && !check_destructor_access(l.type(), b.begin))
      return {NodeId::none, begin, end};
  }
  Ex c = list_init(Types::unqualified(l.type()), b, Construction::copy_list);
  if (!c.ok()) return {NodeId::none, begin, end};
  ExprInfo info{l.type(), ValueCategory::lvalue, 0, l.info.bit_width};
  auto op = class_copy ? ast::AssignOp::trivial_class_copy : ast::AssignOp::assign;
  return node(ast::Assign{info, op, at(l, begin), at(c, begin), Types::unqualified(l.type())},
              begin, end, info);
}

auto Sema::supports_class_throw(EntityId cls, Ex value) -> bool {
  if (!w_->target.supports(ast::NativeOperation::hosted_trivial_class_throw) ||
      value.cat() != ValueCategory::prvalue || !ordinary_default_member_owner(cls)) return false;
  const auto& entity = w_->entities.rec(cls);
  if (entity.linkage != ast::Linkage::external) return false;
  for (EntityId scope = entity.scope; scope != EntityId::none; scope = w_->entities.rec(scope).scope)
    if (ins().names.spelling(w_->entities.rec(scope).name) == "(anonymous)") return false;

  // The original braces must explicitly initialize the field. Normalized lists can also
  // contain synthesized initializers for omitted fields.
  while (auto paren = as<ast::Paren>(value)) {
    value.begin += paren->operand.at;
    value.id = paren->operand.id;
  }
  const auto init = explicit_aggregate_prvalues_.find(value.begin);
  if (init == explicit_aggregate_prvalues_.end() || init->second.id != value.id ||
      list_elements(init->second).size() != 1) return false;

  const auto* record = class_rec(cls);
  if (!record || !record->member_declarations_valid || record->union_ || record->closure ||
      record->layout_requires_virtual_support || record->has_explicit_alignment() || record->unapplied_layout ||
      !record->bases.empty() || record->fields.size() != 1 || record->user_ctors ||
      record->destructor_declaration() != EntityId::none || !record->destructor_usable) return false;
  // [except.throw]: construction from const T& in an unrelated context must also work.
  // With only the ordinary int field, the eligible implicit copy takes const T& and is public.
  const auto& copy = record->trivial_copyability;
  if (copy.copy_constructor != CopyOperation::eligible_trivial ||
      copy.destructor != CopyOperation::eligible_trivial) return false;
  const auto& field = record->fields.front();
  if (types().canonical(field.type) != builtin(Builtin::int_) ||
      record->access_of(field.entity) != ast::Access::public_ || field.dmi ||
      field.bit_width || field.no_unique_address) return false;
  const auto* implicit = record->implicit_copy_assignment();
  return std::ranges::none_of(record->members, [&](const MemberDeclRec& m) {
    return &m != implicit && ins().names.spelling(m.name) == "operator=";
  });
}

auto Sema::throw_expression(std::optional<Ex> operand, u32 begin, u32 end) -> Ex {
  Ex fail{NodeId::none, begin, end};
  if (!check_exception_construct(begin, lex::Tk::kw_throw)) return fail;
  ExprInfo info{builtin(Builtin::void_), ValueCategory::prvalue, 0};
  if (!operand) return node(ast::Throw{info, {}}, begin, end, info);
  Ex value = *operand;
  if (!value.ok()) return fail;
  EntityId exception_class = EntityId::none;
  if (dependent(value)) {
    info.flags |= expr_flag::dependent;
  } else if (types().is_class(value.type())) {
    if (!require_complete(value.type(), value.begin)) return fail;
    exception_class = types().class_of(value.type());
    if (!supports_class_throw(exception_class, value)) {
      diag(value.begin, Diag::throw_class_unsupported, value.type());
      return fail;
    }
  } else {
    if (compatibility() == ast::Compatibility::gcc && types().is_array(value.type()) &&
        !require_complete(value.type(), value.begin))
      return fail;
    value = rvalue(value);
    if (!value.ok() || !require_complete(value.type(), value.begin)) return fail;
    if (types().is_pointer(value.type())) {
      QualType pointee = types().pointee(value.type());
      if (!types().is_void(pointee) && !require_complete(pointee, value.begin)) return fail;
    }
  }
  return node(ast::Throw{info, at(value, begin), exception_class}, begin, end, info);
}

auto Sema::structural_conditional(Ex cond, Ex t, Ex f, u32 q_at) -> Ex {
  const u32 begin = cond.begin, end = f.end;
  const bool is_dependent = dependent(cond) || dependent(t) || dependent(f);
  const QualType left = types().canonical(t.type()), right = types().canonical(f.type());
  const QualType a = Types::unqualified(left), b = Types::unqualified(right);
  const Cv left_cv = Types::cv(left), right_cv = Types::cv(right);
  const bool left_to_right = cv_within(left_cv, right_cv);
  const bool glvalue = t.glvalue() && t.cat() == f.cat() && a == b &&
                       (left_to_right || cv_within(right_cv, left_cv));
  if (glvalue && (t.info.bit_width || f.info.bit_width)) {
    diag(q_at, Diag::bit_field_conditional);
    return {NodeId::none, begin, end};
  }
  QualType result = a == b ? a : integral_common_type(t, f);
  if (glvalue) result = left_to_right ? right : left;
  ExprInfo info{result, glvalue ? t.cat() : ValueCategory::prvalue, expr_flag::structural_projection};
  if (is_dependent) {
    const auto c = structural_conditional_operand(cond);
    const auto x = structural_conditional_operand(t), y = structural_conditional_operand(f);
    if (!c || !x || !y) {
      diag(q_at, Diag::not_supported, lex::Tk::question);
      return {NodeId::none, begin, end};
    }
    const std::array operands{*c, *x, *y};
    const auto value = ins().intern_value(ast::VDepConditional{result, b_->seq(std::span<const TemplateArg>(operands))});
    info.flags |= expr_flag::dependent;
    return node(ast::Literal{info, value}, begin, end, info);
  }

  // Form the ordinary conversions without folding an unchosen static constant reference.
  auto load = [&](Ex e) {
    return e.glvalue() ? convert_node(e, ast::ConversionKind::lvalue_to_rvalue, object_unqualified(e.type())) : e;
  };
  Ex c = load(cond);
  if (!types().is(c.type(), Builtin::bool_)) c = arithmetic_conversion(c, builtin(Builtin::bool_));
  if (glvalue) {
    if (left != result) t = convert_node(t, ast::ConversionKind::qualification, result, t.cat());
    if (right != result) f = convert_node(f, ast::ConversionKind::qualification, result, f.cat());
  } else {
    t = load(t);
    f = load(f);
    if (a != b) arithmetic_conversions(t, f);
  }
  return node(ast::Conditional{info, at(c, begin), at(t, begin), at(f, begin)}, begin, end, info);
}

auto Sema::conditional(Ex cond, Ex t, Ex f, u32 q_at, bool retain_class_parameter) -> Ex {
  constexpr u8 unchecked_copy = ast::Conditional::unchecked_class_copy;
  u32 begin = cond.begin, end = f.end;
  if (!cond.ok() || !t.ok() || !f.ok()) return {NodeId::none, begin, end};
  auto throws = [&](Ex arm) {
    for (;;) {
      if (auto p = as<ast::Paren>(arm)) {
        arm = Ex{p->operand.id};
      } else if (auto c = as<ast::ImplicitConversion>(arm); c && c->conversion == ast::ConversionKind::to_void &&
                 compatibility() == ast::Compatibility::gcc) {
        arm = Ex{c->operand.id};
      } else {
        break;
      }
    }
    return as<ast::Throw>(arm).has_value();
  };
  const bool throws_then = throws(t);
  const bool throws_else = throws(f);
  // [expr.cond]/2: a throw arm leaves the other arm's type and value category intact.
  if (throws_then || throws_else) {
    Ex c = to_bool(cond, cond.begin);
    if (!c.ok()) return {NodeId::none, begin, end};
    ExprInfo info = throws_then ? f.info : t.info;
    info.flags &= ~unchecked_copy;
    if (dependent(c) || dependent(t) || dependent(f)) info.flags |= expr_flag::dependent;
    return node(ast::Conditional{info, at(c, begin), at(t, begin), at(f, begin)}, begin, end, info);
  }
  if (retain_class_parameter &&
      ((cond.info.flags | t.info.flags | f.info.flags) & expr_flag::structural_projection) &&
      types().is_integral(cond.type()) && types().is_integral(t.type()) && types().is_integral(f.type()))
    return structural_conditional(cond, t, f, q_at);

  if (dependent(cond) || dependent(t) || dependent(f)) {
    if (has_proven_dependent_type(cond) || has_proven_dependent_type(t) || has_proven_dependent_type(f))
      return dependent_node({cond, t, f}, begin, end, std::nullopt, ValueCategory::prvalue, expr_flag::type_dependent);
    // Concrete prvalue arms can determine the result type even when the value is dependent.
    if (!types().is_dependent(cond.type()) &&
        t.cat() == ValueCategory::prvalue && f.cat() == ValueCategory::prvalue) {
      if (!types().is_dependent(t.type()) && types().canonical(t.type()) == types().canonical(f.type()))
        return dependent_node({cond, t, f}, begin, end, t.type());
      if (types().is_integral(t.type()) && types().is_integral(f.type()))
        return dependent_node({cond, t, f}, begin, end, integral_common_type(t, f));
    }
    return dependent_node({cond, t, f}, begin, end);
  }
  Ex c = to_bool(cond, cond.begin);
  if (!c.ok()) return {NodeId::none, begin, end};
  ExprInfo info;
  // [expr.cond]/4.1: glvalues of one category, of a class and its base class: the derived one
  // converted to the base, as reference binding does.
  if (t.glvalue() && t.cat() == f.cat() && types().is_class(t.type()) && types().is_class(f.type())) {
    if (base_depth(t.type(), f.type()) && cv_within(object_cv(t.type()), object_cv(f.type())))
      t = to_base(t, types().class_of(f.type()), q_at);
    else if (base_depth(f.type(), t.type()) && cv_within(object_cv(f.type()), object_cv(t.type())))
      f = to_base(f, types().class_of(t.type()), q_at);
    if (!t.ok() || !f.ok()) return {NodeId::none, begin, end};
  }
  if (t.glvalue() && t.cat() == f.cat() && t.type() == f.type()) {
    if (t.info.bit_width || f.info.bit_width) {
      diag(q_at, Diag::bit_field_conditional);
      return {NodeId::none, begin, end};
    }
    info = t.info;
    info.flags &= ~unchecked_copy;
  } else if (t.glvalue() != f.glvalue() &&
             (t.cat() == ValueCategory::lvalue || f.cat() == ValueCategory::lvalue) &&
             types().is_class(t.type()) && same_unqualified(t.type(), f.type()) &&
             object_cv(t.type()) == Cv::none && object_cv(f.type()) == Cv::none) {
    // [expr.cond]/4: convert the lvalue to the other arm's class prvalue by copy
    // initialization. The conditional's operand is an ordinary lvalue.
    Ex& source = t.glvalue() ? t : f;
    const QualType type = object_unqualified(t.type());
    source = construct(type, {source}, source.begin, source.end, Construction::copy);
    if (!source.ok()) return {NodeId::none, begin, end};
    info = {type, ValueCategory::prvalue, 0};
  } else {
    const bool unchecked_class_copy = (t.glvalue() && types().is_class(t.type())) ||
                                      (f.glvalue() && types().is_class(f.type()));
    Ex a = rvalue(t), b = rvalue(f);
    QualType ta = a.type(), tb = b.type();
    if (ta == tb) {
      // [expr.cond]/7.1: the same type, no further conversion.
      info = {ta, ValueCategory::prvalue, unchecked_class_copy ? unchecked_copy : u8{0}};
    } else if (types().is_void(ta) && types().is_void(tb)) {
      info = {ta, ValueCategory::prvalue, 0};
    } else if (types().is_arithmetic(ta) && types().is_arithmetic(tb)) {
      info = {arithmetic_conversions(a, b), ValueCategory::prvalue, 0};
    } else if (types().is_pointer(ta) && (tb == ta || is_null_pointer_constant(f))) {
      if (tb != ta) b = convert_node(b, ast::ConversionKind::null_pointer, ta);
      info = {ta, ValueCategory::prvalue, 0};
    } else if (types().is_pointer(tb) && is_null_pointer_constant(t)) {
      a = convert_node(a, ast::ConversionKind::null_pointer, tb);
      info = {tb, ValueCategory::prvalue, 0};
    } else if (auto to = composite_void_pointer(ta, tb)) {
      a = convert(a, *to, q_at);
      b = convert(b, *to, q_at);
      if (!a.ok() || !b.ok()) return {NodeId::none, begin, end};
      info = {*to, ValueCategory::prvalue, 0};
    } else if (auto to = composite_function_pointer(ta, tb)) {
      a = convert(a, *to, q_at);
      b = convert(b, *to, q_at);
      if (!a.ok() || !b.ok()) return {NodeId::none, begin, end};
      info = {*to, ValueCategory::prvalue, 0};
    } else if (types().is_pointer(ta) && types().is_pointer(tb) &&
               (base_depth(types().pointee(ta), types().pointee(tb)) || base_depth(types().pointee(tb), types().pointee(ta)))) {
      // [expr.cond]/7.3: the composite pointer type, a pointer to the base class.
      bool down = base_depth(types().pointee(ta), types().pointee(tb));
      if (down) a = convert(a, tb, q_at);
      else b = convert(b, ta, q_at);
      if (!a.ok() || !b.ok()) return {NodeId::none, begin, end};
      info = {down ? tb : ta, ValueCategory::prvalue, 0};
    } else if (types().is_pointer(ta) && types().is_pointer(tb) &&
               (qualification_convertible(ta, tb) || qualification_convertible(tb, ta))) {
      QualType to = qualification_convertible(ta, tb) ? tb : ta;
      if (ta != to) a = convert_node(a, ast::ConversionKind::qualification, to);
      if (tb != to) b = convert_node(b, ast::ConversionKind::qualification, to);
      info = {to, ValueCategory::prvalue, 0};
    } else {
      diag(q_at, Diag::incompatible_operands, t.type(), f.type());
      return {NodeId::none, begin, end};
    }
    t = a;
    f = b;
  }
  return node(ast::Conditional{info, at(c, begin), at(t, begin), at(f, begin)}, begin, end, info);
}

auto Sema::adl_associations(std::span<const Ex> args, bool at_instantiation, bool ordinary_call) -> AdlAssociations {
  AdlAssociations result;
  auto add_namespace = [&](EntityId entity) {
    EntityId ns = enclosing_namespace(entity);
    if (!std::ranges::contains(result.namespaces, ns)) result.namespaces.push_back(ns);
  };
  auto add_class = [&](EntityId cls) {
    // A class template specialization is associated too: its friends are found once it is
    // instantiated, which `adl_class` does where it can be ([temp.inst]/2, #259).
    const EntityRec& entity = w_->entities.rec(cls);
    if (entity.kind == EntityKind::class_template) return;
    if (!std::ranges::contains(result.classes, cls)) result.classes.push_back(cls);
  };
  // [basic.lookup.argdep]/3: bases contribute their entities. Expanding a base's template
  // arguments requires a separate occurrence of its type in the argument walk.
  std::vector<EntityId> base_walk;
  auto collect_bases = [&](this auto&& visit, EntityId cls) -> void {
    if (!ordinary_call && w_->entities.rec(cls).templ != EntityId::none) return;
    if (std::ranges::contains(base_walk, cls)) return;
    base_walk.push_back(cls);
    if (const ClassRec* c = q_->adl_class(cls, at_instantiation)) {
      for (const BaseRec& b : c->bases) {
        add_namespace(b.cls);
        add_class(b.cls);
        visit(b.cls);
      }
    }
  };
  std::vector<QualType> expanded_types;
  auto collect = [&](this auto&& visit, QualType t) -> void {
    t = Types::unqualified(types().canonical(t));
    if (std::ranges::contains(expanded_types, t)) return;
    expanded_types.push_back(t);
    TypeRec r = types().rec(t);
    switch (r.kind) {
    case TypeKind::class_:
    case TypeKind::enum_: {
      add_namespace(r.entity);
      const EntityRec& entity = w_->entities.rec(r.entity);
      if (entity.scope != EntityId::none && w_->entities.rec(entity.scope).kind == EntityKind::class_)
        add_class(entity.scope);
      if (r.kind == TypeKind::class_) {
        add_class(r.entity);
        collect_bases(r.entity);
      }
      auto visit_arg = [&](const TemplateArg& arg) {
        if (arg.kind == ast::TemplateArgKind::type) {
          visit(arg.type);
        } else if (ordinary_call && arg.kind == ast::TemplateArgKind::template_ && arg.templ != EntityId::none) {
          // [basic.lookup.argdep]/3: associate the type template itself, not a specialization.
          const EntityRec& declaration = w_->entities.rec(arg.templ);
          if (compatibility() == ast::Compatibility::clang && declaration.kind == EntityKind::alias_template) return;
          const EntityId scope = declaration.scope;
          if (scope == EntityId::none || w_->entities.rec(scope).kind == EntityKind::namespace_)
            add_namespace(arg.templ);
        }
      };
      for (const TemplateArg& arg : entity.args) {
        if (arg.kind == ast::TemplateArgKind::pack)
          std::ranges::for_each(ins().seqs.elements(arg.elems), visit_arg);
        else
          visit_arg(arg);
      }
      break;
    }
    case TypeKind::function:
      for (QualType p : r.params) visit(p);
      visit(r.inner);
      break;
    case TypeKind::member_pointer:
      visit(r.member_owner);
      visit(r.inner);
      break;
    case TypeKind::pointer:
    case TypeKind::array:
    case TypeKind::reference:
      visit(r.inner);
      break;
    default: break;
    }
  };
  for (const Ex& a : args) collect(a.type());
  return result;
}

auto Sema::with_adl(Ex callee, NameId name, std::span<const Ex> args, bool dependent_call) -> Ex {
  std::vector<ast::DeclSema> set;
  if (auto it = unqualified_.find(std::to_underlying(callee.id)); it != unqualified_.end()) set = it->second;
  bool changed = false;
  auto default_count = [&](const ast::DeclSema& d) {
    return std::ranges::count_if(found_defaults(d), [](NodeId n) { return n != NodeId::none; });
  };
  auto add_candidate = [&](const ast::DeclSema& d) {
    auto prior = std::ranges::find(set, d.entity, &ast::DeclSema::entity);
    if (prior == set.end()) {
      set.push_back(d);
      changed = true;
    } else if (d.params != prior->params && default_count(d) > default_count(*prior)) {
      *prior = d;
      changed = true;
    }
  };
  auto add = [&](bool at_instantiation) {
    auto associations = adl_associations(args, at_instantiation, !dependent_call);
    for (EntityId ns : associations.namespaces)
      for (const ast::DeclSema& d : q_->adl_functions(ns, name, at_instantiation)) add_candidate(d);
    for_each_associated_friend(associations, name, at_instantiation, add_candidate);
  };
  add(false);
  if (dependent_call) add(true);
  if (!changed) return callee;
  ExprInfo info{builtin(Builtin::overload_set), ValueCategory::lvalue, 0};
  Ex r = node(ast::DeclRef{info, set.front().entity}, callee.begin, callee.end, info);
  if (auto ex = explicit_args_.find(std::to_underlying(callee.id)); ex != explicit_args_.end())
    explicit_args_[std::to_underlying(r.id)] = ex->second;
  overloads_[std::to_underlying(r.id)] = std::move(set);
  return r;
}

auto Sema::initializer_list_argument(QualType t, const Braced& list) -> std::optional<Ex> {
  const auto element = initializer_list_element(t);
  if (element != builtin(Builtin::int_)) return std::nullopt;

  const QualType const_element = types().add_cv(*element, Cv::const_);
  const QualType pointer = types().pointer(const_element);
  const QualType size_type = builtin(w_->target.size_type);
  const ClassRec* record = class_rec(types().class_of(t));
  if (!record) return std::nullopt;
  for (const MemberDeclRec& member : record->members) {
    if (!member.is_ctor || member.is_template || member.is_deleted) continue;
    const TypeRec fn = types().rec(member.type);
    if (fn.variadic || fn.params.size() != 2 || types().canonical(fn.params[0]) != pointer ||
        types().canonical(fn.params[1]) != size_type)
      continue;
    const EntityId constructor = member.entity;
    const QualType array = types().array(const_element, list.clauses.size());
    Ex backing = list_init(array, list);
    if (!backing.ok()) return backing;
    Ex first = rvalue(materialize(backing, array, false));
    Ex size = literal(size_type, w_->values.integer(size_type, list.clauses.size()),
                      ValueCategory::prvalue, list.begin, list.end);
    ExprInfo info{Types::unqualified(t), ValueCategory::prvalue, 0};
    const std::array args{at(first, list.begin), at(size, list.begin)};
    return node(ast::CtorCall{info, constructor, 0, b_->list(args)}, list.begin, list.end, info);
  }
  return std::nullopt;
}

auto Sema::incomplete_class_member(NameId name, u32 begin, u32 end, u32 name_at, bool unqualified, bool callee, std::optional<u32> address_at) -> std::optional<Ex> {
  const auto bases = class_prefix_bases(current_class);
  if (unqualified && !bases.empty()) {
    // This declaration's earlier declarators, and the current one once typed, precede bases.
    if (const auto* d = own_initializer_static(name))
      return static_member(current_class, d->entity, d->type, begin, end);
    if (name == static_initializer_name_) {
      diag(begin, Diag::auto_used_in_initializer, name);
      return Ex{NodeId::none, begin, end};
    }
  }
  const bool assertion = in_class_assertion();
  std::vector<ast::DeclSema> functions;
  if (auto m = q_->earlier_member(name, /*type_only=*/false, &functions)) {
    if (m->kind == ast::DeclKind::enumerator) return enumerator(m->entity, m->type, begin, end);
    // Rank mixed overloads before checking whether the selected member needs an object.
    if (m->kind == ast::DeclKind::function) {
      const auto is_static = [](const ast::DeclSema& d) { return (d.flags & decl_flag::static_) != 0; };
      if (!functions.empty() && std::ranges::all_of(functions, is_static))
        return decls_expr(std::move(functions), true, name, begin, end, callee);
      if (std::ranges::any_of(functions, is_static))
        return bind_prefix_member_functions(current_class, name, functions, begin, end);
      if (is_static(*m)) return decls_expr({*m}, true, name, begin, end, callee);
    }
    if (m->kind == ast::DeclKind::data_member) {
      // An address needs the field's index in the class record.
      if (address_at) {
        diag(begin, Diag::incomplete_type, types().class_type(current_class));
        return Ex{NodeId::none, begin, end};
      }
      if (unevaluated && (nested_enum_members_begin || assertion))
        return unevaluated_data_member(m->entity, m->type, m->bit_width, begin, end);
    }
    if (m->kind != ast::DeclKind::variable) {
      diag(begin, Diag::member_without_object, name);
      return Ex{NodeId::none, begin, end};
    }
    return static_member(current_class, m->entity, m->type, begin, end);
  }
  if (!bases.empty()) {
    if (w_->entities.rec(current_class).name == name) {
      diag(begin, Diag::type_as_expression, name);
      return Ex{NodeId::none, begin, end};
    }
    if (auto m = class_member(current_class, name, begin, end, name_at, address_at, unqualified,
                              /*check_all_bases=*/true, bases)) return *m;
  }
  // A nondependent base participates in lookup while a class template (or a class nested in one)
  // is being defined ([temp.dep.type]/3).
  if (unqualified && bases.empty() && (templated() || w_->entities.rec(current_class).templ != EntityId::none)) {
    std::vector<BaseRec> nondep;
    for (const BaseRec& base : q_->defining_base_records(current_class))
      if (!base.dependent_in_pattern) nondep.push_back(base);
    if (!nondep.empty())
      if (auto m = class_member(current_class, name, begin, end, name_at, address_at, unqualified,
                                /*check_all_bases=*/true, nondep)) return *m;
  }
  return std::nullopt;
}

}  // namespace ycxx::sema
