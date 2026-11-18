// `lower`: L1 to L2 (optimization design §2.1, §2.3's last column; #26). C++ is erased:
//   - C++ scalar types become `iN` and `ptr` by their size from `layout`; `bool` is `i1` as a
//     value and one byte in memory;
//   - access paths and pointer arithmetic become byte offsets from `layout`;
//   - automatic and temporary `alloc`s become `alloca` frame objects, an `erroneous` or `zero`
//     initial state becomes stores of zero (the erroneous value is zero on every target), an
//     `indeterminate` one nothing;
//   - lifetime markers, `union.activate` and unsequenced-group markers are dropped (the slice
//     gives every frame object its own slot, so there is no stack-slot liveness to record yet);
//   - `is_const_eval` is `false` and `consteval_br` takes its runtime edge; blocks only the
//     consteval edge reached are dropped;
//   - a result narrower than `config.min_result_bits` is returned extended to it and truncated by
//     the caller; arguments and parameters keep their width, and a call records which arguments
//     are signed, so code generation can extend them as the calling convention places them
//     (Apple arm64 extends register arguments to 32 bits but packs stack arguments by size), and
//     how many of them the callee's ellipsis matches, which the calling convention may place
//     apart (Apple arm64 puts them on the stack).
//
// This is the α-slice subset (epic #39): what C-like functions need. Any other op is an error
// naming the node, not a silent miscompile. Aggregates are memory only: `alloc`, access paths and
// `copy` (δ, #86). Not yet: `switch`, EH, coroutines,
// vptr loads, poison for indeterminate reads beyond "no store".
export module ycxx.ir.lower;

import std;
import ycxx.ir;
import ycxx.ir.l2;

namespace ycxx::ir {

// What `lower` needs from the `layout` query (AST design Part D) until types and constants (#33)
// provide it. Each answer is a pure function of its key.
export enum class TypeKind : std::uint8_t { void_, boolean, integer, pointer, array, record, function, other };

export struct TypeLayout {
  TypeKind kind = TypeKind::other;
  std::uint64_t size = 0;
  std::uint32_t align = 1;
  bool is_signed = false;  // integers
  QualType pointee{};      // pointers: the pointed-to type
};

export struct SubObject {
  std::uint64_t offset = 0;  // from the start of the enclosing object
  QualType type{};
};

// A constant's value: an integral, bool or null pointer value, or the address of an entity's
// object (a variable, a string literal) plus a byte offset. An integer converts to one.
export struct ConstValue {
  std::int64_t bits = 0;            // the value, or the offset from `base`
  std::optional<EntityId> base{};   // an address constant
  constexpr ConstValue(std::int64_t v = 0) : bits(v) {}
  constexpr ConstValue(EntityId e, std::int64_t offset) : bits(offset), base(e) {}
};

export template <class L>
concept layout_source = requires(const L& l, QualType t, PathStep s, ConstId c) {
  { l.type(t) } -> std::convertible_to<TypeLayout>;
  { l.step(t, s) } -> std::convertible_to<SubObject>;     // [class.mem], [dcl.array]
  { l.constant(c) } -> std::convertible_to<ConstValue>;
};

// Target facts `lower` reads (the `config` query).
export struct LowerConfig {
  std::uint32_t min_result_bits = 32;  // narrower integer results are returned extended to this
};

export struct LowerError {
  NodeId node{};
  std::string message;
};

export template <layout_source L>
std::expected<l2::Function, LowerError> lower(const Function& f, EntityId name, const L& layout,
                                              const LowerConfig& config = {});

}  // namespace ycxx::ir

namespace ycxx::ir {

template <layout_source L>
class Lowering {
 public:
  Lowering(const Function& f, EntityId name, const L& layout, const LowerConfig& config)
      : f_(f), name_(name), layout_(layout), config_(config), map_(f.values.size(), ValueId(none)) {}

  std::expected<l2::Function, LowerError> run() {
    reachable();
    for (BlockId b : f_.block_ids()) {
      if (block_map_[raw(b)] == none) continue;
      if (auto e = block(b); !e) return std::unexpected(e.error());
    }
    return b_.finish(results_.value_or(std::vector<l2::Type>{}));
  }

 private:
  using T = l2::Type;
  using Op2 = l2::Op;

  const Function& f_;
  EntityId name_;
  const L& layout_;
  LowerConfig config_;
  l2::Builder b_;
  std::vector<ValueId> map_;  // L1 value → L2 value
  std::vector<std::uint32_t> block_map_;
  std::optional<std::vector<T>> results_;
  NodeId at_{};                   // the L1 node being lowered

  // Blocks reachable from the entry, `consteval_br` taking only its runtime edge.
  void reachable() {
    std::vector<std::uint8_t> seen(f_.blocks.size());
    std::vector<std::uint32_t> work{0};
    seen[0] = 1;
    while (!work.empty()) {
      std::uint32_t b = work.back();
      work.pop_back();
      NodeId t = f_.terminator(BlockId(b));
      auto succs = f_.succs_of(t);
      for (std::uint32_t i = 0; i < succs.size(); ++i) {
        if (f_.node(t).op == Op::consteval_br && i == 0) continue;
        std::uint32_t s = raw(succs[i].target);
        if (!seen[s]) {
          seen[s] = 1;
          work.push_back(s);
        }
      }
    }
    block_map_.assign(f_.blocks.size(), none);
    std::uint32_t next = 0;
    for (std::uint32_t b = 0; b < f_.blocks.size(); ++b)
      if (seen[b]) block_map_[b] = next++;
  }

  std::unexpected<LowerError> fail(std::string why) const { return std::unexpected(LowerError{at_, std::move(why)}); }

  TypeLayout layout(QualType t) const { return layout_.type(t); }

  std::expected<T, std::string> type_of(QualType t) const {
    TypeLayout l = layout(t);
    switch (l.kind) {
      case TypeKind::boolean: return T::i1;
      case TypeKind::pointer: return T::ptr;
      case TypeKind::integer:
        if (auto i = l2::int_of_bits(l.size * 8); i && *i != T::i1) return *i;
        return std::unexpected(std::format("T{}: no integer type of {} bytes", raw(t), l.size));
      default: return std::unexpected(std::format("T{}: not a scalar type (not in the α subset)", raw(t)));
    }
  }
  // The type of a value in memory: `bool` is a byte.
  static T memory_type(T t) { return t == T::i1 ? T::i8 : t; }

  QualType cxx(ValueId v) const { return f_.value(v).type.type; }
  bool is_signed(ValueId v) const { return layout(cxx(v)).is_signed; }

  std::array<Origin, 1> origin() const { return {Origin{name_, at_}}; }

  ValueId emit1(Op2 op, T t, std::initializer_list<ValueId> ops, std::initializer_list<std::uint64_t> imms = {},
                Flags flags = 0) {
    auto o = origin();
    return b_.value(op, t, ops, imms, flags, o);
  }
  ValueId constant(T t, std::int64_t v) { return emit1(Op2::const_, t, {}, {std::uint64_t(normalize(t, v))}); }

  // Sign-extends `v` from the width of `t`, so equal constants have equal immediates.
  static std::int64_t normalize(T t, std::int64_t v) {
    std::uint32_t n = l2::bits(t);
    if (n == 0 || n >= 64) return v;
    if (n == 1) return v & 1;
    std::uint64_t m = (std::uint64_t(1) << n) - 1;
    std::uint64_t u = std::uint64_t(v) & m;
    if (u >> (n - 1)) u |= ~m;
    return std::int64_t(u);
  }

  // Integer `v` of type `from` to type `to`: extended by `sign`, or truncated.
  ValueId resize(ValueId v, T to, bool sign) {
    T from = b_.peek().type(v);
    if (from == to) return v;
    if (to == T::i1) return emit1(Op2::icmp, T::i1, {v, constant(from, 0)}, {std::to_underlying(Pred::ne)});
    if (l2::bits(from) < l2::bits(to)) return emit1(from == T::i1 || !sign ? Op2::zext : Op2::sext, to, {v});
    return emit1(Op2::trunc, to, {v});
  }

  // The ABI form of a result: integers narrower than `min_result_bits` extended to it.
  std::optional<T> abi_type(T t) const {
    if (l2::is_int(t) && l2::bits(t) < config_.min_result_bits) return l2::int_of_bits(config_.min_result_bits);
    return std::nullopt;
  }

  // The byte offset of `path` inside an object of type `t`.
  std::uint64_t offset(QualType t, std::span<const std::uint64_t> path) const {
    std::uint64_t off = 0;
    for (std::uint64_t w : path) {
      SubObject s = layout_.step(t, decode_step(w));
      off += s.offset;
      t = s.type;
    }
    return off;
  }
  ValueId add_offset(ValueId p, std::uint64_t off) {
    if (off == 0) return p;
    return emit1(Op2::ptradd, T::ptr, {p, constant(T::i64, std::int64_t(off))});
  }
  // The path immediate starting at word `i` of `imms`: its length, then its steps.
  static std::span<const std::uint64_t> path_at(std::span<const std::uint64_t> imms, std::size_t i) {
    return imms.subspan(i + 1, imms[i]);
  }

  std::expected<void, LowerError> block(BlockId b) {
    std::vector<T> params;
    for (ValueId p : f_.params_of(b)) {
      if (f_.value(p).type.kind == TyKind::state) {
        params.push_back(T::state);
        continue;
      }
      if (f_.value(p).type.kind != TyKind::cxx) return fail("exception and coroutine values are not in the α subset");
      auto t = type_of(cxx(p));
      if (!t) return fail(t.error());
      params.push_back(*t);
    }
    BlockId nb = b_.begin_block(params);
    std::uint32_t i = 0;
    for (ValueId p : f_.params_of(b)) map_[raw(p)] = b_.param(nb, i++);
    for (NodeId n : f_.nodes_of(b)) {
      at_ = n;
      if (auto e = node(n); !e) return e;
    }
    return {};
  }

  ValueId in(NodeId n, std::size_t i) const { return map_[raw(f_.operands_of(n)[i])]; }
  void out(NodeId n, std::size_t i, ValueId v) { map_[f_.node(n).res0 + i] = v; }
  ValueId l1_result(NodeId n, std::size_t i = 0) const { return ValueId(f_.node(n).res0 + i); }

  std::expected<void, LowerError> node(NodeId n) {
    const Node& nd = f_.node(n);
    auto imms = f_.imms_of(n);
    auto result_type = [&]() { return type_of(cxx(l1_result(n))); };
    auto arith = [&](Op2 op) -> std::expected<void, LowerError> {
      auto t = result_type();
      if (!t) return fail(t.error());
      out(n, 0, emit1(op, *t, {in(n, 0), in(n, 1)}, {}, nd.flags));
      return {};
    };
    switch (nd.op) {
      case Op::const_: {
        auto t = result_type();
        if (!t) return fail(t.error());
        ConstValue c = layout_.constant(ConstId(std::uint32_t(imms[0])));
        if (c.base) {  // the address of an object, as a string literal's is
          if (*t != T::ptr) return fail("an address constant must have a pointer type");
          out(n, 0, add_offset(emit1(Op2::symbol, T::ptr, {}, {raw(*c.base)}), std::uint64_t(c.bits)));
          return {};
        }
        out(n, 0, constant(*t, c.bits));
        return {};
      }
      case Op::global:
      case Op::func_addr: out(n, 0, emit1(Op2::symbol, T::ptr, {}, {imms[0]})); return {};
      case Op::is_const_eval: out(n, 0, constant(T::i1, 0)); return {};

      case Op::add: return arith(Op2::add);
      case Op::sub: return arith(Op2::sub);
      case Op::mul: return arith(Op2::mul);
      case Op::sdiv: return arith(Op2::sdiv);
      case Op::udiv: return arith(Op2::udiv);
      case Op::srem: return arith(Op2::srem);
      case Op::urem: return arith(Op2::urem);
      case Op::and_: return arith(Op2::and_);
      case Op::or_: return arith(Op2::or_);
      case Op::xor_: return arith(Op2::xor_);
      case Op::shl:
      case Op::lshr:
      case Op::ashr: {
        // The amount takes the shifted operand's type; amounts are never negative without UB.
        auto t = result_type();
        if (!t) return fail(t.error());
        ValueId amount = resize(in(n, 1), *t, false);
        Op2 op = nd.op == Op::shl ? Op2::shl : nd.op == Op::lshr ? Op2::lshr : Op2::ashr;
        out(n, 0, emit1(op, *t, {in(n, 0), amount}, {}, nd.flags));
        return {};
      }
      case Op::neg: {
        auto t = result_type();
        if (!t) return fail(t.error());
        out(n, 0, emit1(Op2::neg, *t, {in(n, 0)}, {}, nd.flags));
        return {};
      }
      case Op::icmp: out(n, 0, emit1(Op2::icmp, T::i1, {in(n, 0), in(n, 1)}, {imms[0]})); return {};
      case Op::ptr_cmp: {
        // Pointers compare as addresses, unsigned ([expr.rel]: only related pointers are ordered).
        Pred p = Pred(imms[0]);
        switch (p) {
          case Pred::lt: case Pred::slt: p = Pred::ult; break;
          case Pred::le: case Pred::sle: p = Pred::ule; break;
          case Pred::gt: case Pred::sgt: p = Pred::ugt; break;
          case Pred::ge: case Pred::sge: p = Pred::uge; break;
          default: break;
        }
        out(n, 0, emit1(Op2::icmp, T::i1, {in(n, 0), in(n, 1)}, {std::to_underlying(p)}));
        return {};
      }
      case Op::select: {
        auto t = result_type();
        if (!t) return fail(t.error());
        out(n, 0, emit1(Op2::select, *t, {in(n, 0), in(n, 1), in(n, 2)}));
        return {};
      }
      case Op::iconv:
      case Op::tobool: {
        auto t = result_type();
        if (!t) return fail(t.error());
        ValueId v = in(n, 0);
        if (b_.peek().type(v) == T::ptr) {
          if (*t != T::i1) return fail("a pointer converts only to bool here");
          out(n, 0, emit1(Op2::icmp, T::i1, {v, constant(T::ptr, 0)}, {std::to_underlying(Pred::ne)}));
          return {};
        }
        out(n, 0, resize(v, *t, is_signed(f_.operands_of(n)[0])));
        return {};
      }
      case Op::ptr_cast: out(n, 0, in(n, 0)); return {};
      case Op::ptr_to_int: {
        auto t = result_type();
        if (!t) return fail(t.error());
        out(n, 0, resize(emit1(Op2::ptr2int, T::i64, {in(n, 0)}), *t, false));
        return {};
      }
      case Op::int_to_ptr: {
        ValueId v = resize(in(n, 0), T::i64, is_signed(f_.operands_of(n)[0]));
        out(n, 0, emit1(Op2::int2ptr, T::ptr, {v}));
        return {};
      }
      case Op::ptr_add: {
        std::uint64_t size = layout(QualType(std::uint32_t(imms[0]))).size;
        ValueId i = resize(in(n, 1), T::i64, is_signed(f_.operands_of(n)[1]));
        if (size != 1) i = emit1(Op2::mul, T::i64, {i, constant(T::i64, std::int64_t(size))}, {}, bit(Flag::nsw));
        out(n, 0, emit1(Op2::ptradd, T::ptr, {in(n, 0), i}));
        return {};
      }
      case Op::ptr_diff: {
        auto t = result_type();
        if (!t) return fail(t.error());
        std::uint64_t size = layout(QualType(std::uint32_t(imms[0]))).size;
        ValueId a = emit1(Op2::ptr2int, T::i64, {in(n, 0)});
        ValueId c = emit1(Op2::ptr2int, T::i64, {in(n, 1)});
        ValueId d = emit1(Op2::sub, T::i64, {a, c}, {}, bit(Flag::nsw));
        if (size > 1) d = emit1(Op2::sdiv, T::i64, {d, constant(T::i64, std::int64_t(size))}, {}, bit(Flag::exact));
        out(n, 0, resize(d, *t, true));
        return {};
      }
      case Op::ptr_member: {
        QualType base = layout(cxx(f_.operands_of(n)[0])).pointee;
        out(n, 0, add_offset(in(n, 0), offset(base, path_at(imms, 1))));
        return {};
      }

      case Op::alloc: {
        auto storage = Storage(imms[1]);
        if (storage != Storage::automatic && storage != Storage::temporary)
          return fail(std::format("{} storage is not in the α subset", ir::name_of(storage)));
        TypeLayout l = layout(QualType(std::uint32_t(imms[0])));
        ValueId p = emit1(Op2::alloca, T::ptr, {}, {l.size, l.align, raw(n)});
        ValueId s = in(n, 0);
        if (Init(imms[2]) != Init::indeterminate) s = zero(s, p, l, imms[0]);
        out(n, 0, s);
        out(n, 1, p);
        return {};
      }
      case Op::dealloc:
      case Op::obj__begin:
      case Op::obj__end:
      case Op::union__activate:
      case Op::useq__begin:
      case Op::useq__next:
      case Op::useq__end:
        if (nd.op == Op::dealloc && Storage(imms[0]) != Storage::automatic && Storage(imms[0]) != Storage::temporary)
          return fail("only automatic deallocation is in the α subset");
        out(n, 0, in(n, 0));
        return {};
      case Op::copy: {
        // A trivial copy ([class.copy.ctor], [basic.types.general]): the object's bytes, in the
        // widest chunks its alignment allows.
        TypeLayout l = layout(QualType(std::uint32_t(imms[0])));
        out(n, 0, copy_bytes(in(n, 0), in(n, 1), in(n, 2), l, imms[0]));
        return {};
      }
      case Op::load: {
        QualType t = QualType(std::uint32_t(imms[0]));
        auto vt = type_of(t);
        if (!vt) return fail(vt.error());
        QualType base = layout(cxx(f_.operands_of(n)[1])).pointee;
        ValueId p = add_offset(in(n, 1), offset(base, path_at(imms, 1)));
        ValueId v = emit1(Op2::load, memory_type(*vt), {in(n, 0), p}, {imms[0]}, nd.flags);
        if (*vt == T::i1) v = emit1(Op2::trunc, T::i1, {v});
        out(n, 0, v);
        return {};
      }
      case Op::store: {
        QualType t = QualType(std::uint32_t(imms[0]));
        auto vt = type_of(t);
        if (!vt) return fail(vt.error());
        QualType base = layout(cxx(f_.operands_of(n)[1])).pointee;
        ValueId p = add_offset(in(n, 1), offset(base, path_at(imms, 1)));
        ValueId v = in(n, 2);
        if (*vt == T::i1) v = emit1(Op2::zext, T::i8, {v});
        auto o = origin();
        T st[] = {T::state};
        ValueId ops[] = {in(n, 0), p, v};
        std::uint64_t im[] = {imms[0]};
        out(n, 0, b_.peek().result(b_.emit(Op2::store, st, ops, im, nd.flags, {}, o)));
        return {};
      }
      case Op::call:
      case Op::call_ind: {
        bool ind = nd.op == Op::call_ind;
        std::vector<ValueId> ops{in(n, 0)};
        auto l1ops = f_.operands_of(n);
        if (ind) ops.push_back(in(n, 1));
        std::uint64_t sext = 0;
        for (std::size_t i = ind ? 2 : 1, k = 0; i < l1ops.size(); ++i, ++k) {
          ValueId v = map_[raw(l1ops[i])];
          if (k >= 32) return fail("more than 32 arguments are not in the α subset");
          if (b_.peek().type(v) != T::i1 && b_.peek().type(v) != T::ptr && is_signed(l1ops[i])) sext |= 1u << k;
          ops.push_back(v);
        }
        std::vector<T> res{T::state};
        std::vector<std::optional<T>> narrow;
        for (std::uint32_t i = 1; i < nd.nres; ++i) {
          auto t = type_of(cxx(l1_result(n, i)));
          if (!t) return fail(t.error());
          auto abi = abi_type(*t);
          res.push_back(abi.value_or(*t));
          narrow.push_back(abi ? std::optional(*t) : std::nullopt);
        }
        auto o = origin();
        std::vector<std::uint64_t> im;
        if (!ind) im.push_back(imms[0]);
        im.push_back(sext);
        im.push_back(imms[ind ? 0 : 1]);  // the arguments the ellipsis matches
        NodeId c = b_.emit(ind ? Op2::call_ind : Op2::call, res, ops, im, 0, {}, o);
        out(n, 0, b_.peek().result(c));
        for (std::uint32_t i = 1; i < nd.nres; ++i) {
          ValueId v = b_.peek().result(c, i);
          // Only the result's own bits are read: a bool is bit 0, whatever the callee left above it.
          if (narrow[i - 1]) v = emit1(Op2::trunc, *narrow[i - 1], {v});
          out(n, i, v);
        }
        return {};
      }

      case Op::br:
      case Op::cond_br:
      case Op::consteval_br: {
        std::vector<l2::Builder::SuccArgs> succs;
        auto l1succs = f_.succs_of(n);
        for (std::uint32_t i = 0; i < l1succs.size(); ++i) {
          if (nd.op == Op::consteval_br && i == 0) continue;
          auto& s = succs.emplace_back(BlockId(block_map_[raw(l1succs[i].target)]));
          for (ValueId a : f_.args_of(l1succs[i])) s.args.push_back(map_[raw(a)]);
        }
        auto o = origin();
        std::vector<ValueId> ops;
        if (nd.op == Op::cond_br) ops.push_back(in(n, 0));
        b_.emit(nd.op == Op::cond_br ? Op2::cond_br : Op2::br, {}, ops, {}, 0, succs, o);
        return {};
      }
      case Op::ret: {
        std::vector<ValueId> ops{in(n, 0)};
        std::vector<T> types;
        auto l1ops = f_.operands_of(n);
        for (std::size_t i = 1; i < l1ops.size(); ++i) {
          ValueId v = map_[raw(l1ops[i])];
          T t = b_.peek().type(v);
          if (auto abi = abi_type(t)) v = resize(v, *abi, t != T::i1 && is_signed(l1ops[i]));
          ops.push_back(v);
          types.push_back(b_.peek().type(v));
        }
        if (results_ && *results_ != types) return fail("returns differ in type");
        results_ = types;
        auto o = origin();
        b_.emit(Op2::ret, {}, ops, {}, 0, {}, o);
        return {};
      }
      case Op::unreachable: {
        auto o = origin();
        b_.emit(Op2::unreachable, {}, {}, {}, 0, {}, o);
        return {};
      }
      default: return fail(std::format("`{}` is not in the α subset", info(nd.op).name));
    }
  }

  // Copies the object at `src` to `dst`, chunk by chunk.
  ValueId copy_bytes(ValueId s, ValueId dst, ValueId src, const TypeLayout& l, std::uint64_t alias) {
    std::uint64_t off = 0;
    while (off < l.size) {
      std::uint64_t w = 8;
      while (w > 1 && (w > l.size - off || off % w || w > l.align)) w /= 2;
      T t = *l2::int_of_bits(w * 8);
      auto o = origin();
      ValueId v = b_.value(Op2::load, t, {s, add_offset(src, off)}, {alias}, 0, o);
      T st[] = {T::state};
      ValueId ops[] = {s, add_offset(dst, off), v};
      std::uint64_t im[] = {alias};
      s = b_.peek().result(b_.emit(Op2::store, st, ops, im, 0, {}, o));
      off += w;
    }
    return s;
  }

  // Stores zero over the object at `p` (the erroneous value is zero on every target, and `zero`
  // initialization is zero), in the widest chunks its alignment allows.
  ValueId zero(ValueId s, ValueId p, const TypeLayout& l, std::uint64_t alias) {
    std::uint64_t off = 0;
    while (off < l.size) {
      std::uint64_t w = 8;
      while (w > 1 && (w > l.size - off || off % w || w > l.align)) w /= 2;
      T t = *l2::int_of_bits(w * 8);
      ValueId at = add_offset(p, off);
      auto o = origin();
      T st[] = {T::state};
      ValueId ops[] = {s, at, constant(t, 0)};
      std::uint64_t im[] = {alias};
      s = b_.peek().result(b_.emit(Op2::store, st, ops, im, 0, {}, o));
      off += w;
    }
    return s;
  }
};

template <layout_source L>
std::expected<l2::Function, LowerError> lower(const Function& f, EntityId name, const L& layout,
                                              const LowerConfig& config) {
  return Lowering<L>(f, name, layout, config).run();
}

}  // namespace ycxx::ir
