// `legalize`: L2 to L2, expanding the 128-bit operations a target lacks (#1172). `lower` writes
// `i128` operations for every target; here each one the target's `Int128Support` names is kept,
// and every other is expanded:
//   - a value is its two 64-bit halves. A target with 128-bit registers (`values`) keeps values
//     whole, and only an unsupported operation takes them apart (`trunc`, `high`) and joins its
//     result (`pair`); without them, no `i128` value is left;
//   - addition, subtraction and multiplication carry between the halves; bitwise operations,
//     `select` and block arguments go half by half; a shift combines the halves, choosing by the
//     amount's bit 6; a comparison reads the low halves when the high ones are equal;
//   - a load or store is two, low half first (both targets are little-endian);
//   - division, remainder and the floating conversions call the compiler runtime (compiler-rt,
//     libgcc), whose functions take and return 128-bit integers in pairs of 64-bit words. A call
//     joins the block's `!state` chain where the operation stood.
// A function without an `i128` value is returned as it is.
export module ycxx.ir.legalize;

import std;
import ycxx.ir;
import ycxx.ir.l2;

namespace ycxx::ir {

// The runtime functions an expansion calls, in the order of `runtime_names`.
export enum class Runtime : std::uint8_t {
  sdiv, udiv, srem, urem,
  f32_to_s128, f64_to_s128, f32_to_u128, f64_to_u128,
  s128_to_f32, s128_to_f64, u128_to_f32, u128_to_f64,
};
export constexpr std::array<std::string_view, 12> runtime_names{
    "__divti3",    "__udivti3",   "__modti3",      "__umodti3",     "__fixsfti",     "__fixdfti",
    "__fixunssfti", "__fixunsdfti", "__floattisf", "__floattidf", "__floatuntisf", "__floatuntidf"};
static_assert(runtime_names.size() == std::meta::enumerators_of(^^Runtime).size());
export constexpr std::string_view name_of(Runtime r) { return runtime_names[std::to_underlying(r)]; }

// What a target does with 128-bit values itself. The default is nothing: AArch64's and x86-64's
// general registers hold 64 bits.
export struct Int128Support {
  bool values = false;                 // registers hold them: block arguments, constants, `pair`, `high`, width changes
  std::bitset<l2::op_count> ops{};     // and these operations, if `values`

  bool has(l2::Op op) const {
    if (!values) return false;
    switch (op) {
      case l2::Op::const_: case l2::Op::pair: case l2::Op::high: case l2::Op::trunc:
      case l2::Op::br: case l2::Op::cond_br: case l2::Op::call: case l2::Op::call_ind: case l2::Op::ret: return true;
      default: return ops.test(std::to_underlying(op));
    }
  }
};

// The entity an expansion calls for a runtime function.
export using RuntimeEntity = std::function<EntityId(Runtime)>;

export struct LegalizeError {
  std::optional<NodeId> at;  // the L1 node it came from
  std::string message;
};

export std::expected<l2::Function, LegalizeError> legalize(l2::Function f, const Int128Support& target,
                                                           const RuntimeEntity& runtime);

}  // namespace ycxx::ir

namespace ycxx::ir {
namespace {

using l2::Op;
using T = l2::Type;

class Legalize {
 public:
  Legalize(const l2::Function& f, const Int128Support& target, const RuntimeEntity& runtime)
      : f_(f), target_(target), runtime_(runtime), map_(f.values.size()) {}

  std::expected<l2::Function, LegalizeError> run() {
    for (BlockId b : f_.block_ids()) block(b);
    if (error_) return std::unexpected(std::move(*error_));
    std::vector<T> results = f_.results;
    if (std::ranges::contains(results, T::i128) && !target_.values)
      return std::unexpected(LegalizeError{std::nullopt, "an i128 result"});
    l2::Function out = b_.finish(std::move(results));
    out.indirect_result = f_.indirect_result;
    return out;
  }

 private:
  // A value of the input: itself in the output, or the halves of an i128.
  struct Value {
    ValueId whole = ValueId(none);
    std::array<ValueId, 2> half{ValueId(none), ValueId(none)};
    std::optional<std::int64_t> constant;  // a `const`'s value, for shift amounts
  };
  using Halves = std::array<ValueId, 2>;

  const l2::Function& f_;
  const Int128Support& target_;
  const RuntimeEntity& runtime_;
  l2::Builder b_;
  std::vector<Value> map_;
  std::optional<LegalizeError> error_;
  NodeId at_ = NodeId(none);
  // The block's latest `!state` in the input: what a runtime call follows. The states a call in this
  // block replaced, to restore where the block does not dominate.
  ValueId state_ = ValueId(none);
  std::vector<std::pair<ValueId, ValueId>> replaced_;

  bool wide(ValueId v) const { return f_.type(v) == T::i128; }
  void fail(std::string why) {
    if (error_) return;
    std::optional<NodeId> l1;
    if (raw(at_) != none && !origin().empty()) l1 = origin().front().node;
    error_ = LegalizeError{l1, std::move(why)};
  }

  std::span<const Origin> origin() const { return f_.origins.size() ? f_.origins.of(at_) : std::span<const Origin>{}; }
  ValueId emit(Op op, T t, std::initializer_list<ValueId> ops, std::initializer_list<std::uint64_t> imms = {}, Flags flags = 0) {
    return b_.value(op, t, ops, imms, flags, origin());
  }
  ValueId i64(Op op, ValueId a, ValueId c) { return emit(op, T::i64, {a, c}); }
  ValueId constant(std::int64_t v) { return emit(Op::const_, T::i64, {}, {std::uint64_t(v)}); }
  ValueId icmp(Pred p, ValueId a, ValueId c) { return emit(Op::icmp, T::i1, {a, c}, {std::to_underlying(p)}); }
  ValueId select(ValueId c, ValueId a, ValueId d) { return emit(Op::select, T::i64, {c, a, d}); }

  Halves halves(ValueId v) {
    const Value& m = map_[raw(v)];
    if (raw(m.half[0]) != none) return m.half;
    // Taken apart at each use: a half made here need not dominate another block's use.
    return {emit(Op::trunc, T::i64, {m.whole}), emit(Op::high, T::i64, {m.whole})};
  }
  ValueId whole(ValueId v) const { return map_[raw(v)].whole; }
  void set(ValueId v, ValueId out) { map_[raw(v)].whole = out; }
  // The halves of `v`, joined when the target holds it whole.
  void set(ValueId v, Halves h) {
    if (target_.values) set(v, emit(Op::pair, T::i128, {h[0], h[1]}));
    else map_[raw(v)].half = h;
  }
  // `v` in a value list: itself or its halves.
  void append(std::vector<ValueId>& out, ValueId v) const {
    if (wide(v) && !target_.values) out.append_range(map_[raw(v)].half);
    else out.push_back(whole(v));
  }

  void block(BlockId b) {
    std::vector<T> params;
    for (ValueId p : f_.params_of(b)) {
      if (!wide(p) || target_.values) {
        params.push_back(f_.type(p));
      } else if (raw(b) == 0) {
        return fail("an i128 parameter");  // `lower` passes words
      } else {
        params.insert(params.end(), 2, T::i64);
      }
    }
    BlockId nb = b_.begin_block(params);
    std::uint32_t i = 0;
    state_ = ValueId(none);
    replaced_.clear();
    for (ValueId p : f_.params_of(b)) {
      if (wide(p) && !target_.values) {
        map_[raw(p)].half = {b_.param(nb, i), b_.param(nb, i + 1)};
        i += 2;
        continue;
      }
      set(p, b_.param(nb, i++));
      if (f_.type(p) == T::state) state_ = p;
    }
    for (NodeId n : f_.nodes_of(b)) {
      at_ = n;
      if (error_) return;
      node(n);
      const l2::Node& nd = f_.node(n);
      if (raw(state_) == none)  // a block without a state parameter reads a dominating one
        for (ValueId v : f_.operands_of(n))
          if (f_.type(v) == T::state) state_ = v;
      for (std::uint32_t r = 0; r < nd.nres; ++r)
        if (f_.type(f_.result(n, r)) == T::state) state_ = f_.result(n, r);
    }
    for (auto [state, before] : replaced_ | std::views::reverse) set(state, before);
  }

  void node(NodeId n) {
    const l2::Node& nd = f_.node(n);
    const auto ops = f_.operands_of(n);
    bool any = std::ranges::any_of(ops, [&](ValueId v) { return wide(v); });
    for (std::uint32_t r = 0; r < nd.nres; ++r) any |= wide(f_.result(n, r));
    for (const Succ& s : f_.succs_of(n))
      for (ValueId v : f_.args_of(s)) any |= wide(v);
    if (nd.op == Op::const_) map_[raw(f_.result(n))].constant = std::int64_t(f_.imms_of(n)[0]);
    if (!any || target_.has(nd.op)) return keep(n);
    expand(n);
  }

  // The node as it is, its operands mapped.
  void keep(NodeId n) {
    const l2::Node& nd = f_.node(n);
    std::vector<T> results;
    for (std::uint32_t r = 0; r < nd.nres; ++r) results.push_back(f_.type(f_.result(n, r)));
    std::vector<ValueId> operands;
    for (ValueId v : f_.operands_of(n)) operands.push_back(whole(v));
    terminator_or_node(n, results, operands);
  }
  void terminator_or_node(NodeId n, std::span<const T> results, std::span<const ValueId> operands) {
    const l2::Node& nd = f_.node(n);
    std::vector<l2::Builder::SuccArgs> succs;
    for (const Succ& s : f_.succs_of(n)) {
      auto& to = succs.emplace_back(s.target);
      for (ValueId v : f_.args_of(s)) append(to.args, v);
    }
    NodeId m = b_.emit(nd.op, results, operands, f_.imms_of(n), nd.flags, succs, origin());
    for (std::uint32_t r = 0; r < nd.nres; ++r) set(f_.result(n, r), b_.peek().result(m, r));
  }

  // A runtime call where the operation stands: after the block's latest state, which later uses
  // of that state then follow.
  std::vector<ValueId> call(Runtime fn, std::span<const ValueId> args, std::span<const T> results) {
    if (raw(state_) == none) {
      // The operation comes before the block's first effect: its state is that effect's operand.
      const std::uint32_t block = f_.node(at_).block;
      for (NodeId n : f_.nodes_of(BlockId(block))) {
        for (ValueId v : f_.operands_of(n))
          if (raw(state_) == none && f_.type(v) == T::state) state_ = v;
        for (const Succ& s : f_.succs_of(n))
          for (ValueId v : f_.args_of(s))
            if (raw(state_) == none && f_.type(v) == T::state) state_ = v;
        if (raw(state_) != none) break;
      }
      if (raw(state_) == none) {
        fail("a runtime call in a block without a state");
        return {};
      }
    }
    std::vector<ValueId> operands{whole(state_)};
    operands.append_range(args);
    std::vector<T> types{T::state};
    types.append_range(results);
    const std::uint64_t imms[] = {raw(runtime_(fn)), 0, 0, 0};
    NodeId m = b_.emit(Op::call, types, operands, imms, 0, {}, origin());
    // A state another block defined is this one's again after it: the block need not dominate
    // its other uses.
    if (defining_block(state_) != f_.node(at_).block) replaced_.push_back({state_, whole(state_)});
    set(state_, b_.peek().result(m));
    std::vector<ValueId> out;
    for (std::uint32_t r = 0; r < results.size(); ++r) out.push_back(b_.peek().result(m, 1 + r));
    return out;
  }
  std::uint32_t defining_block(ValueId v) const {
    const l2::Value& d = f_.value(v);
    return d.param ? d.def : f_.node(NodeId(d.def)).block;
  }

  // Division and remainder: two 128-bit operands.
  void divide(NodeId n, Runtime fn) {
    Halves a = halves(f_.operands_of(n)[0]), c = halves(f_.operands_of(n)[1]);
    const ValueId args[] = {a[0], a[1], c[0], c[1]};
    const T words[] = {T::i64, T::i64};
    if (auto r = call(fn, args, words); !r.empty()) set(f_.result(n), Halves{r[0], r[1]});
  }

  void expand(NodeId n) {
    const l2::Node& nd = f_.node(n);
    const auto ops = f_.operands_of(n);
    const auto imms = f_.imms_of(n);
    const ValueId result = nd.nres ? f_.result(n) : ValueId(none);
    auto two = [&] { return std::pair(halves(ops[0]), halves(ops[1])); };
    switch (nd.op) {
      case Op::const_: {
        const auto low = std::int64_t(imms[0]);
        return set(result, Halves{constant(low), constant(low < 0 ? -1 : 0)});
      }
      case Op::pair: return set(result, Halves{whole(ops[0]), whole(ops[1])});
      case Op::high: return set(result, halves(ops[0])[1]);
      case Op::add: {
        auto [a, c] = two();
        ValueId low = i64(Op::add, a[0], c[0]);
        ValueId carry = emit(Op::zext, T::i64, {icmp(Pred::ult, low, a[0])});
        return set(result, Halves{low, i64(Op::add, i64(Op::add, a[1], c[1]), carry)});
      }
      case Op::sub: {
        auto [a, c] = two();
        return set(result, subtract(a, c));
      }
      case Op::neg: {
        ValueId zero = constant(0);
        return set(result, subtract({zero, zero}, halves(ops[0])));
      }
      case Op::mul: {
        auto [a, c] = two();
        // Each node has its own statement, here and below: arguments evaluate in no fixed order.
        ValueId low = i64(Op::mul, a[0], c[0]);
        ValueId carry = i64(Op::umulh, a[0], c[0]);
        ValueId cross = i64(Op::mul, a[0], c[1]);
        ValueId cross2 = i64(Op::mul, a[1], c[0]);
        ValueId high = i64(Op::add, cross, cross2);
        return set(result, Halves{low, i64(Op::add, carry, high)});
      }
      case Op::and_: case Op::or_: case Op::xor_: {
        auto [a, c] = two();
        return set(result, Halves{i64(nd.op, a[0], c[0]), i64(nd.op, a[1], c[1])});
      }
      case Op::shl: case Op::lshr: case Op::ashr: return set(result, shift(nd.op, halves(ops[0]), ops[1]));
      case Op::icmp: {
        auto [a, c] = two();
        return set(result, compare(Pred(imms[0]), a, c));
      }
      case Op::select: {
        Halves a = halves(ops[1]), c = halves(ops[2]);
        ValueId if_ = whole(ops[0]);
        return set(result, Halves{select(if_, a[0], c[0]), select(if_, a[1], c[1])});
      }
      case Op::zext: {
        ValueId low = whole(ops[0]);
        if (f_.type(ops[0]) != T::i64) low = emit(Op::zext, T::i64, {low});
        return set(result, Halves{low, constant(0)});
      }
      case Op::sext: {
        ValueId low = whole(ops[0]);
        if (f_.type(ops[0]) != T::i64) low = emit(Op::sext, T::i64, {low});
        return set(result, Halves{low, i64(Op::ashr, low, constant(63))});
      }
      case Op::trunc: {
        ValueId low = halves(ops[0])[0];
        return set(result, f_.type(result) == T::i64 ? low : emit(Op::trunc, f_.type(result), {low}));
      }
      case Op::sdiv: return divide(n, Runtime::sdiv);
      case Op::udiv: return divide(n, Runtime::udiv);
      case Op::srem: return divide(n, Runtime::srem);
      case Op::urem: return divide(n, Runtime::urem);
      case Op::fptosi: case Op::fptoui: {
        const bool s = nd.op == Op::fptosi, f32 = f_.type(ops[0]) == T::f32;
        const ValueId args[] = {whole(ops[0])};
        const T words[] = {T::i64, T::i64};
        auto r = call(f32 ? (s ? Runtime::f32_to_s128 : Runtime::f32_to_u128) : s ? Runtime::f64_to_s128 : Runtime::f64_to_u128,
                      args, words);
        if (!r.empty()) set(result, Halves{r[0], r[1]});
        return;
      }
      case Op::sitofp: case Op::uitofp: {
        const bool s = nd.op == Op::sitofp, f32 = f_.type(result) == T::f32;
        Halves a = halves(ops[0]);
        const T types[] = {f_.type(result)};
        auto r = call(f32 ? (s ? Runtime::s128_to_f32 : Runtime::u128_to_f32) : s ? Runtime::s128_to_f64 : Runtime::u128_to_f64,
                      a, types);
        if (!r.empty()) set(result, r[0]);
        return;
      }
      case Op::load: {
        ValueId p = whole(ops[1]), state = whole(ops[0]);
        ValueId low = emit(Op::load, T::i64, {state, p}, {imms[0]}, nd.flags);
        return set(result, Halves{low, emit(Op::load, T::i64, {state, upper(p)}, {imms[0]}, nd.flags)});
      }
      case Op::store: {
        ValueId p = whole(ops[1]);
        Halves v = halves(ops[2]);
        ValueId state = emit(Op::store, T::state, {whole(ops[0]), p, v[0]}, {imms[0]}, nd.flags);
        return set(result, emit(Op::store, T::state, {state, upper(p), v[1]}, {imms[0]}, nd.flags));
      }
      case Op::br: case Op::cond_br: {
        std::vector<ValueId> operands;
        for (ValueId v : ops) operands.push_back(whole(v));
        return terminator_or_node(n, {}, operands);
      }
      default: return fail(std::format("an i128 `{}`", l2::info(nd.op).name));
    }
  }

  ValueId upper(ValueId p) { return emit(Op::ptradd, T::ptr, {p, constant(8)}); }

  Halves subtract(Halves a, Halves c) {
    ValueId borrow = emit(Op::zext, T::i64, {icmp(Pred::ult, a[0], c[0])});
    return {i64(Op::sub, a[0], c[0]), i64(Op::sub, i64(Op::sub, a[1], c[1]), borrow)};
  }

  ValueId compare(Pred p, Halves a, Halves c) {
    if (p == Pred::eq || p == Pred::ne) {
      ValueId x = i64(Op::xor_, a[0], c[0]);
      ValueId y = i64(Op::xor_, a[1], c[1]);
      ValueId differ = i64(Op::or_, x, y);
      return icmp(p, differ, constant(0));
    }
    Pred low = p;  // the low halves are unsigned
    switch (p) {
      case Pred::slt: low = Pred::ult; break;
      case Pred::sle: low = Pred::ule; break;
      case Pred::sgt: low = Pred::ugt; break;
      case Pred::sge: low = Pred::uge; break;
      default: break;
    }
    ValueId equal = icmp(Pred::eq, a[1], c[1]);
    ValueId by_low = icmp(low, a[0], c[0]);
    ValueId by_high = icmp(p, a[1], c[1]);
    return emit(Op::select, T::i1, {equal, by_low, by_high});
  }

  // A shift by `amount`, below 128 (more is poison). The bits that cross the halves are moved by
  // 1 and then by 63 less the amount's low six bits, so that a zero amount moves none.
  Halves shift(Op op, Halves v, ValueId amount) {
    const bool left = op == Op::shl;
    const ValueId near = left ? v[0] : v[1], far = left ? v[1] : v[0];  // bits move from `near` into `far`
    auto result = [&](ValueId n, ValueId f) { return left ? Halves{n, f} : Halves{f, n}; };
    const Op back = left ? Op::lshr : Op::shl;
    const Op into = left ? Op::shl : Op::lshr;
    // What fills the vacated half.
    auto fill = [&] { return op == Op::ashr ? i64(Op::ashr, near, constant(63)) : constant(0); };
    if (auto k = map_[raw(amount)].constant; k && *k >= 0 && *k < 128) {
      if (*k == 0) return v;
      if (*k == 64) return result(fill(), near);
      if (*k > 64) {
        ValueId vacated = fill();
        return result(vacated, i64(op, near, constant(*k - 64)));
      }
      ValueId moved = i64(op, near, constant(*k));
      ValueId kept = i64(into, far, constant(*k));
      ValueId across = i64(back, near, constant(64 - *k));
      return result(moved, i64(Op::or_, kept, across));
    }
    const ValueId low = halves(amount)[0];
    const ValueId k = i64(Op::and_, low, constant(63));
    const ValueId by_one = i64(back, near, constant(1));
    const ValueId rest = i64(Op::xor_, k, constant(63));
    const ValueId across = i64(back, by_one, rest);
    const ValueId moved = i64(op, near, k);
    const ValueId kept = i64(into, far, k);
    const ValueId joined = i64(Op::or_, kept, across);
    const ValueId bit = i64(Op::and_, low, constant(64));
    const ValueId big = icmp(Pred::ne, bit, constant(0));
    const ValueId vacated = fill();
    const ValueId n = select(big, vacated, moved);
    return result(n, select(big, moved, joined));
  }
};

}  // namespace

std::expected<l2::Function, LegalizeError> legalize(l2::Function f, const Int128Support& target,
                                                    const RuntimeEntity& runtime) {
  if (!std::ranges::contains(f.values, T::i128, &l2::Value::type)) return f;
  return Legalize(f, target, runtime).run();
}

}  // namespace ycxx::ir
