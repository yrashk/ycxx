// Constant folding of an analysed expression ([expr.const]), for what the α subset needs before
// the interpreter exists (DECISIONS §4.6): array bounds, and the constant initializers of
// namespace-scope variables ([basic.start.static]). It reads the frame's drafts. Anything it
// does not know, and anything with undefined behaviour (signed overflow, division by zero, an
// out-of-range shift), is not a constant. The interpreter replaces it (M4).
export module ycxx.sema:fold;

import std;
import ycxx.ast;
import :types;
import :actions;

export namespace ycxx::sema {

class Folder {
public:
  Folder(Sema& s, Questions& q) : s_(&s), q_(&q) {}

  std::optional<ConstId> fold(const Ex& e) {
    if (!e.ok()) return std::nullopt;
    return fold(e.id);
  }

private:
  Types& types() { return s_->types(); }
  Values& values() { return s_->world().values; }

  template <class T>
  std::optional<T> get(NodeId n) {
    if (n == NodeId::none || ast::kind_of(n) != ast::Nodes::index_of<T>()) return std::nullopt;
    return s_->builder().get<T>(n);
  }

  std::optional<ConstId> integer(QualType t, u64 v) { return values().integer(t, v); }

  std::optional<ConstId> fold(NodeId n) {
    using ast::ConversionKind;
    if (auto l = get<ast::Literal>(n)) return l->value;
    if (auto p = get<ast::Paren>(n)) return fold(p->operand.id);
    if (auto c = get<ast::ImplicitConversion>(n)) {
      if (c->conversion == ConversionKind::lvalue_to_rvalue) {
        if (auto d = get<ast::DeclRef>(c->operand.id)) return q_->constant_value(d->entity);
        return std::nullopt;
      }
      if (c->conversion == ConversionKind::array_to_pointer) {
        // The address of a string literal object's first element.
        auto l = get<ast::Literal>(c->operand.id);
        if (!l || values().rec(l->value).kind != ValueKind::string) return std::nullopt;
        return values().address(c->info.type, l->value, 0);
      }
      auto v = fold(c->operand.id);
      if (!v) return std::nullopt;
      const ValueRec& r = values().rec(*v);
      switch (c->conversion) {
      case ConversionKind::integral: return integer(c->info.type, r.bits);
      case ConversionKind::boolean:
        if (r.kind == ValueKind::integer) return integer(c->info.type, r.bits != 0);
        if (r.kind == ValueKind::null_pointer) return integer(c->info.type, 0);
        if (r.kind == ValueKind::address) return integer(c->info.type, 1);
        return std::nullopt;
      case ConversionKind::null_pointer: return values().null_pointer(c->info.type);
      case ConversionKind::qualification:
        if (r.kind == ValueKind::address) return values().address(c->info.type, r.base, r.offset);
        if (r.kind == ValueKind::null_pointer) return values().null_pointer(c->info.type);
        return std::nullopt;
      default: return std::nullopt;
      }
    }
    if (auto u = get<ast::Unary>(n)) {
      auto v = fold(u->operand.id);
      if (!v || values().rec(*v).kind != ValueKind::integer) return std::nullopt;
      u64 x = values().rec(*v).bits;
      QualType t = u->info.type;
      switch (u->op) {
      case ast::UnaryOp::plus: return integer(t, x);
      case ast::UnaryOp::minus:
        if (shape(*types().builtin_of(t)).is_signed) return checked(t, checked_sub(0, i64(x)));
        return integer(t, u64(0) - x);
      case ast::UnaryOp::compl_: return integer(t, ~x);
      case ast::UnaryOp::not_: return integer(t, x == 0);
      default: return std::nullopt;
      }
    }
    if (auto b = get<ast::Binary>(n)) return binary(*b);
    if (auto o = get<ast::OrderedBinary>(n)) return ordered(*o);
    if (auto c = get<ast::Conditional>(n)) {
      auto v = fold(c->condition.id);
      if (!v) return std::nullopt;
      return fold(values().rec(*v).bits ? c->then_.id : c->else_.id);
    }
    return std::nullopt;
  }

  // A signed result outside its type's range is undefined, so not constant ([expr.pre]/4); an
  // unsigned one wraps ([basic.fundamental]). Operands are at most 64 bits wide.
  static constexpr i64 i64_max = std::numeric_limits<i64>::max();
  static constexpr i64 i64_min = std::numeric_limits<i64>::min();
  static std::optional<i64> checked_add(i64 x, i64 y) {
    if ((y > 0 && x > i64_max - y) || (y < 0 && x < i64_min - y)) return std::nullopt;
    return x + y;
  }
  static std::optional<i64> checked_sub(i64 x, i64 y) {
    if ((y < 0 && x > i64_max + y) || (y > 0 && x < i64_min + y)) return std::nullopt;
    return x - y;
  }
  static std::optional<i64> checked_mul(i64 x, i64 y) {
    if (x == 0 || y == 0) return 0;
    if ((x == -1 && y == i64_min) || (y == -1 && x == i64_min)) return std::nullopt;
    i64 r = static_cast<i64>(static_cast<u64>(x) * static_cast<u64>(y));
    if (r / y != x) return std::nullopt;
    return r;
  }
  std::optional<ConstId> checked(QualType t, std::optional<i64> v) {
    if (!v) return std::nullopt;
    IntShape s = shape(*types().builtin_of(t));
    if (s.bits < 64) {
      i64 max = (i64{1} << (s.bits - 1)) - 1;
      if (*v > max || *v < -max - 1) return std::nullopt;
    }
    return integer(t, u64(*v));
  }

  std::optional<ConstId> binary(const ast::Binary& b) {
    auto l = fold(b.lhs.id), r = fold(b.rhs.id);
    if (!l || !r) return std::nullopt;
    const ValueRec& lr = values().rec(*l);
    const ValueRec& rr = values().rec(*r);
    if (lr.kind != ValueKind::integer || rr.kind != ValueKind::integer) return std::nullopt;
    QualType ot = lr.type;  // the operands' common type
    bool is_signed = shape(*types().builtin_of(ot)).is_signed;
    u64 ux = lr.bits, uy = rr.bits;
    i64 x = i64(ux), y = i64(uy);
    QualType t = b.info.type;
    auto cmp = [&](auto f) { return integer(t, is_signed ? f(x, y) : f(ux, uy)); };
    using enum ast::BinaryOp;
    switch (b.op) {
    case mul: return is_signed ? checked(t, checked_mul(x, y)) : integer(t, ux * uy);
    case add: return is_signed ? checked(t, checked_add(x, y)) : integer(t, ux + uy);
    case sub: return is_signed ? checked(t, checked_sub(x, y)) : integer(t, ux - uy);
    case div:
    case rem:
      if (uy == 0) return std::nullopt;
      if (!is_signed) return integer(t, b.op == div ? ux / uy : ux % uy);
      if (y == -1 && x == i64_min) return std::nullopt;
      return checked(t, b.op == div ? x / y : x % y);
    case lt: return cmp([](auto a, auto c) { return a < c; });
    case gt: return cmp([](auto a, auto c) { return a > c; });
    case le: return cmp([](auto a, auto c) { return a <= c; });
    case ge: return cmp([](auto a, auto c) { return a >= c; });
    case eq: return integer(t, ux == uy);
    case ne: return integer(t, ux != uy);
    case bit_and: return integer(t, ux & uy);
    case bit_xor: return integer(t, ux ^ uy);
    case bit_or: return integer(t, ux | uy);
    default: return std::nullopt;
    }
  }

  std::optional<ConstId> ordered(const ast::OrderedBinary& o) {
    auto l = fold(o.lhs.id);
    if (!l) return std::nullopt;
    using enum ast::OrderedOp;
    if (o.op == logical_and || o.op == logical_or) {
      bool lv = values().rec(*l).bits != 0;
      if (o.op == logical_and && !lv) return integer(o.info.type, 0);
      if (o.op == logical_or && lv) return integer(o.info.type, 1);
      auto r = fold(o.rhs.id);
      if (!r) return std::nullopt;
      return integer(o.info.type, values().rec(*r).bits != 0);
    }
    auto r = fold(o.rhs.id);
    if (!r) return std::nullopt;
    if (o.op == comma) return r;
    if (o.op != shl && o.op != shr) return std::nullopt;
    // [expr.shift]: the right operand must be in [0, width of the promoted left operand).
    QualType t = o.info.type;
    IntShape s = shape(*types().builtin_of(t));
    const ValueRec& rr = values().rec(*r);
    bool neg = shape(*types().builtin_of(rr.type)).is_signed && i64(rr.bits) < 0;
    if (neg || rr.bits >= s.bits) return std::nullopt;
    u64 n = rr.bits;
    u64 x = values().rec(*l).bits;
    if (o.op == shl) return integer(t, x << n);
    return integer(t, s.is_signed ? u64(i64(x) >> n) : (x & mask(s.bits)) >> n);
  }

  static u64 mask(u32 bits) { return bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1; }

  Sema* s_;
  Questions* q_;
};

}  // namespace ycxx::sema
