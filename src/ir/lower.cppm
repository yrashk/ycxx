// `lower`: L1 to L2 (optimization design §2.1, §2.3's last column; #26). C++ is erased:
//   - C++ scalar types become `iN`, `ptr`, `f32` and `f64` by their size from `layout`; `bool` is
//     `i1` as a value and one byte in memory;
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
// `copy` (δ, #86). Native Linux `int` throws use a certified cleanup-free frame (#578).
// Not yet: `switch`, exception handlers and cleanups, coroutines,
// vptr loads, poison for indeterminate reads beyond "no store".
export module ycxx.ir.lower;

import std;
import ycxx.ast.target;
import ycxx.ir;
import ycxx.ir.l2;

namespace ycxx::ir {

// What `lower` needs from the `layout` query (AST design Part D) until types and constants (#33)
// provide it. Each answer is a pure function of its key.
export enum class TypeKind : std::uint8_t { void_, boolean, integer, pointer, array, record, function, other, floating, member_pointer };

export struct TypeLayout {
  TypeKind kind = TypeKind::other;
  std::uint64_t size = 0;
  std::uint32_t align = 1;
  bool is_signed = false;  // integers
  QualType pointee{};      // pointers: the pointed-to type
  // Records: trivially copyable for calls ([class.temporary]/3): a trivial, non-deleted copy or
  // move constructor and destructor, so the target's convention may pass it in registers.
  bool trivial_calls = true;
  bool empty = false;  // records: no non-static data members
  // Records: a homogeneous floating-point aggregate (AAPCS64 §5.9.5): `hfa` members, 1 to 4, at
  // any depth and through arrays, all of one floating type `hfa_bytes` bytes wide; 0 otherwise.
  std::uint8_t hfa = 0;
  std::uint8_t hfa_bytes = 0;
  // Records: the bytes a trivial copy writes, nvsize (Itanium §2.4), none for an empty class: not
  // the tail padding a derived class's members may occupy when the object is a base subobject.
  std::uint64_t data = 0;
  bool contains_member_pointer = false;  // a scalar member pointer or an aggregate containing one
  // Records: the bits of each INTEGER eightbyte in the supported AMD64 slice, zero otherwise.
  std::uint8_t sysv_integer_eightbytes = 0;
  bool copy_abi_unknown = false;  // records whose defaulted copy has unresolved triviality
  bool calls_abi_uncertain = false;  // records whose non-triviality for calls is not established
  bool layout_uncertain = false;     // records whose source layout directives are not applied
  bool contains_explicit_alignment = false;  // a class-head requirement raises this object or a subobject's alignment
};

export struct SubObject {
  std::uint64_t offset = 0;  // from the start of the enclosing object
  QualType type{};
  ast::BitField bit_field{};
};

// A constant's value: an integral, bool or null pointer value, a floating value's encoding, or the
// address of an entity's object (a variable, a string literal) plus a byte offset. An integer
// converts to one.
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

// The Itanium runtime imports for a certified Linux int throw. The type is the normalized
// exception object type, before L2 replaces C++ types with machine widths.
export struct IntThrowRuntime {
  QualType type;
  EntityId allocate;
  EntityId throw_;
  EntityId type_info;
};

export struct CatchAllRuntime {
  EntityId begin;
  EntityId end;
  EntityId resume;
};

// Target and function facts `lower` reads.
export struct LowerConfig {
  std::uint32_t min_result_bits = 32;  // narrower integer results are returned extended to this
  // A class trivially copyable for calls of at most this many bytes passes and returns in that
  // many 8-byte integer registers, rounded up; any other class by the address of an object
  // (AAPCS64 §6.8.2 rules B.3, C.10–C.12, §6.9; Itanium C++ ABI §3.1.2).
  std::uint32_t composite_in_registers = 16;
  // The integer argument registers. A class that needs more of them than are left goes whole on
  // the stack, and the registers left go unused (AAPCS64 C.11): `lower` fills them with words no
  // one reads.
  std::uint32_t int_arg_registers = 8;
  // The floating-point argument registers, likewise for a homogeneous floating-point aggregate,
  // which passes and returns one member per register (AAPCS64 C.2–C.3, §6.9).
  std::uint32_t float_arg_registers = 8;
  // Use the System V AMD64 integer aggregate classification and argument placement.
  bool sysv_amd64 = false;
  std::optional<IntThrowRuntime> int_throw;
  std::optional<CatchAllRuntime> catch_all{};
  EntityId cleanup_resume = EntityId::none;
};

export enum class Amd64ArgumentKind : std::uint8_t { integer, sse, stack };

export struct Amd64ArgumentLocation {
  Amd64ArgumentKind kind;
  std::uint32_t index;
};

// AMD64 psABI §3.2.3. Register classes advance independently. A group of INTEGER
// eightbytes that does not fit uses consecutive stack slots and leaves the GPRs available.
export class Amd64ArgumentPlan {
 public:
  static constexpr std::uint32_t integer_registers = 6;
  static constexpr std::uint32_t sse_registers = 8;

  Amd64ArgumentLocation place(l2::Type type, std::uint32_t register_words = 1,
                              std::uint32_t stack_words = 1) {
    const bool sse = l2::is_float(type);
    auto& used = sse ? sse_used_ : integer_used_;
    const auto limit = sse ? sse_registers : integer_registers;
    if (used + register_words <= limit) {
      const auto first = used;
      used += register_words;
      return {sse ? Amd64ArgumentKind::sse : Amd64ArgumentKind::integer, first};
    }
    const auto first = stack_used_;
    stack_used_ += stack_words;
    return {Amd64ArgumentKind::stack, first};
  }

  std::uint32_t stack_slots() const { return stack_used_; }
  std::uint32_t used_integer_registers() const { return integer_used_; }
  std::uint32_t used_sse_registers() const { return sse_used_; }

 private:
  std::uint32_t integer_used_ = 0;
  std::uint32_t sse_used_ = 0;
  std::uint32_t stack_used_ = 0;
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

// Encode grouped INTEGER arguments in the flat L2 list. Prefix the spills with the
// register arguments of each class, filling unused GPR slots to keep spills on the stack.
// An SSE spill implies all XMM argument registers are already used.
class Amd64ArgumentOrder {
 public:
  Amd64ArgumentLocation add(std::uint32_t first, l2::Type type, std::uint32_t register_words = 1,
                            std::uint32_t stack_words = 1) {
    const auto location = plan_.place(type, register_words, stack_words);
    if (location.kind == Amd64ArgumentKind::stack) {
      for (std::uint32_t w = 0; w < stack_words; ++w) stack_.push_back(first + w);
    } else {
      auto& registers = location.kind == Amd64ArgumentKind::sse ? sse_ : integer_;
      for (std::uint32_t w = 0; w < register_words; ++w) registers.push_back(first + w);
    }
    return location;
  }

  std::vector<std::uint32_t> order() && {
    if (!stack_.empty()) integer_.resize(Amd64ArgumentPlan::integer_registers, none);
    integer_.insert(integer_.end(), sse_.begin(), sse_.end());
    integer_.insert(integer_.end(), stack_.begin(), stack_.end());
    return std::move(integer_);
  }

 private:
  Amd64ArgumentPlan plan_;
  std::vector<std::uint32_t> integer_, sse_, stack_;
};

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
    if (catch_edges_ && !catch_edges_->cleanup) emit_catch_dispatch();
    l2::Function out = b_.finish(results_.value_or(std::vector<l2::Type>{}));
    out.indirect_result = indirect_result_;
    return out;
  }

 private:
  using T = l2::Type;
  using Op2 = l2::Op;

  struct ClassPieces {
    std::uint32_t words = 0;
    T type = T::i64;
    std::uint64_t size = 0;
    std::uint8_t sysv_chunks = 0;

    std::uint64_t offset(std::uint32_t word) const {
      // A single INTEGER eightbyte can follow an initial NO_CLASS eightbyte.
      return (word + (sysv_chunks == 2 ? 1 : 0)) * l2::bytes(type);
    }
    std::uint64_t width(std::uint32_t word) const {
      return std::min<std::uint64_t>(l2::bytes(type), size - offset(word));
    }
  };

  const Function& f_;
  EntityId name_;
  const L& layout_;
  LowerConfig config_;
  l2::Builder b_;
  std::vector<ValueId> map_;  // L1 value → L2 value
  std::vector<std::uint32_t> block_map_;
  struct CatchEdges {
    NodeId invoke;
    BlockId normal, handler;  // L1 blocks
    BlockId landing, resume;  // additional L2 blocks
    bool cleanup = false;
  };
  std::optional<CatchEdges> catch_edges_;
  std::optional<std::vector<T>> results_;
  NodeId at_{};                   // the L1 node being lowered
  // The class result: returned in registers from this frame object, or passed indirectly.
  std::optional<ValueId> result_object_;
  std::uint32_t indirect_result_ = 0;
  ClassPieces result_pieces_;
  std::uint32_t frame_objects_ = 0;  // the ABI's own frame objects, numbered apart from L1's allocs

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
      case TypeKind::member_pointer: return std::unexpected("pointer-to-member values are not supported yet");
      case TypeKind::integer:
        if (auto i = l2::int_of_bits(l.size * 8); i && *i != T::i1) return *i;
        return std::unexpected(std::format("T{}: no integer type of {} bytes", raw(t), l.size));
      case TypeKind::floating:
        if (l.size == 4) return T::f32;
        if (l.size == 8) return T::f64;
        return std::unexpected(std::format("T{}: no floating type of {} bytes", raw(t), l.size));
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

  // Load only the bytes intersecting the field. The enclosing allocation unit can also contain
  // a separate ordinary member, which is a distinct memory location ([intro.memory]).
  ValueId load_field(ValueId state, ValueId base, const SubObject& field) {
    const ast::BitField bits = field.bit_field;
    ValueId packed = constant(T::i32, 0);
    for (std::uint32_t i = 0; i < bits.bytes(); ++i) {
      ValueId p = add_offset(base, field.offset + i);
      ValueId byte = emit1(Op2::load, T::i8, {state, p}, {0});
      ValueId v = resize(byte, T::i32, false);
      if (i != 0) v = emit1(Op2::shl, T::i32, {v, constant(T::i32, 8 * i)});
      packed = emit1(Op2::or_, T::i32, {packed, v});
    }
    if (bits.shift != 0) packed = emit1(Op2::lshr, T::i32, {packed, constant(T::i32, bits.shift)});
    if (bits.width == 32) return packed;
    if (layout(field.type).is_signed) {
      ValueId shift = constant(T::i32, 32 - bits.width);
      ValueId high = emit1(Op2::shl, T::i32, {packed, shift});
      return emit1(Op2::ashr, T::i32, {high, shift});
    }
    return emit1(Op2::and_, T::i32, {packed, constant(T::i32, bits.mask() >> bits.shift)});
  }

  ValueId store_field(ValueId state, ValueId base, const SubObject& field, ValueId value) {
    const ast::BitField bits = field.bit_field;
    if (bits.shift != 0) value = emit1(Op2::shl, T::i32, {value, constant(T::i32, bits.shift)});
    for (std::uint32_t i = 0; i < bits.bytes(); ++i) {
      std::uint8_t mask = std::uint8_t(bits.mask() >> (8 * i));
      ValueId p = add_offset(base, field.offset + i);
      ValueId v = i == 0 ? value : emit1(Op2::lshr, T::i32, {value, constant(T::i32, 8 * i)});
      ValueId byte = resize(v, T::i8, false);
      if (mask != 0xff) {
        ValueId previous = emit1(Op2::load, T::i8, {state, p}, {0});
        previous = emit1(Op2::and_, T::i8, {previous, constant(T::i8, std::uint8_t(~mask))});
        byte = emit1(Op2::and_, T::i8, {byte, constant(T::i8, mask)});
        byte = emit1(Op2::or_, T::i8, {previous, byte});
      }
      auto o = origin();
      T result[] = {T::state};
      ValueId operands[] = {state, p, byte};
      std::uint64_t alias[] = {0};  // character-sized access to the object representation
      state = b_.peek().result(b_.emit(Op2::store, result, operands, alias, 0, {}, o));
    }
    return state;
  }
  // The path immediate starting at word `i` of `imms`: its length, then its steps.
  static std::span<const std::uint64_t> path_at(std::span<const std::uint64_t> imms, std::size_t i) {
    return imms.subspan(i + 1, imms[i]);
  }

  std::expected<void, LowerError> block(BlockId b) {
    bool entry = raw(b) == 0;
    bool place_amd64 = entry && config_.sysv_amd64 && f_.byval_params;
    std::vector<T> params;
    // Words that fill a by-value parameter, or a local destination for a class result.
    struct ClassObject {
      ValueId l1;
      std::uint32_t first;
      bool result;
      ClassPieces pieces;
    };
    std::vector<ClassObject> objects;
    Amd64ArgumentOrder amd64_args;
    std::vector<std::int64_t> l2_of;  // per L1 parameter: its L2 parameter, or -1
    std::uint32_t j = 0;              // the value parameter's index
    // System V AMD64: a class result in memory takes the first INTEGER argument, ahead of
    // `this` and every declared parameter (psABI §3.2.3; Itanium C++ ABI §3.1.3).
    std::optional<ValueId> sysv_result;
    if (entry && config_.sysv_amd64 && f_.result_param) {
      std::uint32_t k = 0;
      for (ValueId p : f_.params_of(b))
        if (f_.value(p).type.kind == TyKind::cxx && ++k == f_.result_param) {
          if (sysv_memory_result(layout(cxx(p)).pointee)) sysv_result = p;
          break;
        }
    }
    for (ValueId p : f_.params_of(b)) {
      if (f_.value(p).type.kind == TyKind::state) {
        l2_of.push_back(std::int64_t(params.size()));
        params.push_back(T::state);
        if (sysv_result && params.size() == 1) {
          indirect_result_ = std::uint32_t(params.size());  // 1 + its index past `!state`
          if (place_amd64) amd64_args.add(std::uint32_t(params.size()), T::ptr);
          params.push_back(T::ptr);
        }
        continue;
      }
      if (sysv_result && p == *sysv_result) {
        ++j;
        l2_of.push_back(std::int64_t(indirect_result_));
        continue;
      }
      if (catch_edges_ && b == catch_edges_->handler && f_.value(p).type.kind == TyKind::exn) {
        l2_of.push_back(std::int64_t(params.size()));
        params.push_back(T::ptr);
        continue;
      }
      if (f_.value(p).type.kind != TyKind::cxx) return fail("exception and coroutine values are not in the α subset");
      bool result = entry && f_.result_param == j + 1, byval = entry && j < 32 && ((f_.byval_params >> j) & 1);
      ++j;
      if (result || byval) {
        QualType pc = layout(cxx(p)).pointee;
        const TypeLayout object = layout(pc);
        if (object.copy_abi_unknown) return fail("defaulted copy ABI classification is unavailable");
        if (!config_.sysv_amd64 && object.contains_explicit_alignment)
          return fail("AArch64 Darwin ABI: explicitly aligned classes by value are not supported yet");
        if (ignored(pc)) {  // passed by nothing: an object of this frame stands for it
          objects.push_back({p, 0, result, {}});
          l2_of.push_back(-1);
          continue;
        }
        ClassPieces pieces = class_pieces(pc, byval ? &amd64_args : nullptr, std::uint32_t(params.size()));
        // A class that is not trivial for calls passes by the address of its object, as on
        // AArch64 (Itanium C++ ABI §3.1.2.3).
        if (config_.sysv_amd64 && !pieces.words && !(byval && !object.trivial_calls))
          return fail("unsupported System V AMD64 aggregate");
        if (result && pieces.words) {
          objects.push_back({p, 0, true, pieces});
          l2_of.push_back(-1);
          continue;
        }
        if (byval && pieces.words) {
          if (!config_.sysv_amd64) {
            const T piece = pieces.type;
            pad_registers(used(params, piece, 1, indirect_result_), pieces.words, piece,
                          [&] { params.push_back(piece); });
          }
          objects.push_back({p, std::uint32_t(params.size()), false, pieces});
          l2_of.push_back(-1);
          params.insert(params.end(), pieces.words, pieces.type);
          continue;
        }
        if (result) indirect_result_ = std::uint32_t(params.size());  // 1 + its index past `!state`
      }
      auto t = type_of(cxx(p));
      if (!t) return fail(t.error());
      if (place_amd64) amd64_args.add(std::uint32_t(params.size()), *t);
      l2_of.push_back(std::int64_t(params.size()));
      params.push_back(*t);
    }
    std::vector<std::uint32_t> index;
    if (place_amd64) {
      index.resize(params.size());
      std::vector<T> ordered{T::state};
      for (std::uint32_t i : std::move(amd64_args).order()) {
        if (i != none) index[i] = std::uint32_t(ordered.size());
        ordered.push_back(i == none ? T::i64 : params[i]);
      }
      params = std::move(ordered);
    }
    const bool normal_invoke = catch_edges_ && b == catch_edges_->normal;
    if (normal_invoke) {
      if (!params.empty()) return fail("hosted invoke normal block must be parameterless in L1");
      params = {T::state};
      if (!catch_edges_->cleanup) params.push_back(T::i32);
    }
    if (catch_edges_ && catch_edges_->cleanup && b == catch_edges_->handler) params.push_back(T::i64);
    BlockId nb = b_.begin_block(params);
    auto param = [&](std::uint32_t i) { return b_.param(nb, index.empty() ? i : index[i]); };
    if (normal_invoke) {
      out(catch_edges_->invoke, 0, param(0));
      if (!catch_edges_->cleanup) out(catch_edges_->invoke, 1, param(1));
    }
    std::uint32_t i = 0;
    for (ValueId p : f_.params_of(b)) {
      if (l2_of[i] >= 0) map_[raw(p)] = param(std::uint32_t(l2_of[i]));
      ++i;
    }
    if (!objects.empty()) {
      // The objects the words fill, or the class result's object, in this frame.
      ValueId state = map_[raw(*f_.params_of(b).begin())];
      for (const ClassObject& r : objects) {
        TypeLayout l = layout(layout(cxx(r.l1)).pointee);
        ValueId obj = frame_object(l);
        if (r.result) {
          result_object_ = obj;
          result_pieces_ = r.pieces;
        } else {
          for (std::uint32_t w = 0; w < r.pieces.words; ++w)
            state = store_class_piece(state, obj, r.pieces, w, param(r.first + w), config_.sysv_amd64);
        }
        map_[raw(r.l1)] = obj;
      }
      map_[raw(*f_.params_of(b).begin())] = state;
    }
    for (NodeId n : f_.nodes_of(b)) {
      at_ = n;
      if (auto e = node(n); !e) return e;
    }
    return {};
  }

  ValueId in(NodeId n, std::size_t i) const { return map_[raw(f_.operands_of(n)[i])]; }
  void out(NodeId n, std::size_t i, ValueId v) { map_[f_.node(n).res0 + i] = v; }
  ValueId l1_result(NodeId n, std::size_t i = 0) const { return ValueId(f_.node(n).res0 + i); }

  // The selector belongs to the machine landing edge. L1 presents only a selected handler
  // and its exception; neither ABI register becomes an ordinary L1 value.
  void emit_catch_dispatch() {
    const auto& edges = *catch_edges_;
    at_ = edges.invoke;
    const auto o = origin();
    const T landing_params[] = {T::state, T::ptr, T::i64};
    const auto landing = b_.begin_block(landing_params);
    const auto state = b_.param(landing, 0), header = b_.param(landing, 1), selector = b_.param(landing, 2);
    const auto matches = emit1(Op2::icmp, T::i1, {selector, constant(T::i64, 1)}, {std::to_underlying(Pred::eq)});
    const l2::Builder::SuccArgs paths[] = {
      {BlockId(block_map_[raw(edges.handler)]), {state, header}},
      {edges.resume, {state, header}},
    };
    b_.emit(Op2::cond_br, {}, std::span(&matches, 1), {}, 0, paths, o);
    const T resume_params[] = {T::state, T::ptr};
    const auto resume = b_.begin_block(resume_params);
    const ValueId args[] = {b_.param(resume, 0), b_.param(resume, 1)};
    const std::uint64_t target[] = {raw(config_.catch_all->resume), 0, 0, 0};
    const T result[] = {T::state};
    b_.emit(Op2::call, result, args, target, 0, {}, o);
    b_.emit(Op2::unreachable, {}, {}, {}, 0, {}, o);
  }

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

      case Op::fadd: return arith(Op2::fadd);
      case Op::fsub: return arith(Op2::fsub);
      case Op::fmul: return arith(Op2::fmul);
      case Op::fdiv: return arith(Op2::fdiv);
      case Op::fneg:
      case Op::fabs: {
        auto t = result_type();
        if (!t) return fail(t.error());
        out(n, 0, emit1(nd.op == Op::fneg ? Op2::fneg : Op2::fabs, *t, {in(n, 0)}));
        return {};
      }
      case Op::fcmp: out(n, 0, emit1(Op2::fcmp, T::i1, {in(n, 0), in(n, 1)}, {imms[0]})); return {};
      case Op::fconv: {
        auto t = result_type();
        if (!t) return fail(t.error());
        ValueId v = in(n, 0);
        T from = b_.peek().type(v);
        if (from != *t) v = emit1(from == T::f32 ? Op2::fpext : Op2::fptrunc, *t, {v});
        out(n, 0, v);
        return {};
      }
      case Op::f2i: {
        // [conv.fpint]: truncated in 32 or 64 bits, then to the type's width; a value the type
        // cannot hold is undefined, so the bits dropped do not matter.
        auto t = result_type();
        if (!t) return fail(t.error());
        if (*t == T::i1) return fail("a floating value converts to bool by `tobool`");
        T wide = l2::bits(*t) <= 32 ? T::i32 : T::i64;
        ValueId v = emit1(is_signed(l1_result(n)) ? Op2::fptosi : Op2::fptoui, wide, {in(n, 0)});
        out(n, 0, resize(v, *t, false));
        return {};
      }
      case Op::i2f: {
        // A narrow integer is first extended to 32 bits by its signedness.
        auto t = result_type();
        if (!t) return fail(t.error());
        ValueId v = in(n, 0);
        bool sign = b_.peek().type(v) != T::i1 && is_signed(f_.operands_of(n)[0]);
        if (l2::bits(b_.peek().type(v)) < 32) v = resize(v, T::i32, sign);
        out(n, 0, emit1(sign ? Op2::sitofp : Op2::uitofp, *t, {v}));
        return {};
      }
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
        if (l2::is_float(b_.peek().type(v))) {  // [conv.bool]: zero of either sign is false, a NaN true
          if (*t != T::i1) return fail("a floating value converts to an integer by `f2i`");
          T ft = b_.peek().type(v);
          out(n, 0, emit1(Op2::fcmp, T::i1, {v, constant(ft, 0)}, {std::to_underlying(Pred::ne)}));
          return {};
        }
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
      case Op::ptr_derived: {
        QualType derived{std::uint32_t(imms[0])};
        out(n, 0, add_offset(in(n, 0), -offset(derived, path_at(imms, 1))));
        return {};
      }

      case Op::alloc: {
        auto storage = Storage(imms[1]);
        if (storage != Storage::automatic && storage != Storage::temporary)
          return fail(std::format("{} storage is not in the α subset", ir::name_of(storage)));
        TypeLayout l = layout(QualType(std::uint32_t(imms[0])));
        if (Init(imms[2]) != Init::indeterminate && l.contains_member_pointer)
          return fail("zero-initializing a pointer-to-member object is not supported yet");
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
      case Op::obj__reuse:
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
        if (l.kind == TypeKind::record) l.size = l.data;
        out(n, 0, copy_bytes(in(n, 0), in(n, 1), in(n, 2), l, imms[0]));
        return {};
      }
      case Op::nullptr_value:
        out(n, 0, constant(T::ptr, 0));
        return {};
      case Op::load: {
        QualType t = QualType(std::uint32_t(imms[0]));
        auto vt = type_of(t);
        if (!vt) return fail(vt.error());
        QualType base = layout(cxx(f_.operands_of(n)[1])).pointee;
        ValueId p = add_offset(in(n, 1), offset(base, path_at(imms, 1)));
        ValueId v = emit1(Op2::load, memory_type(*vt), {in(n, 0), p}, {imms[0]}, nd.flags & bit(Flag::volatile_));
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
      case Op::va_start:
      case Op::va_arg: {
        auto o = origin();
        if (!config_.sysv_amd64 && nd.op == Op::va_arg) {
          // Apple arm64: the cursor holds the address of the next argument, each in an 8-byte
          // slot of its own, the value in its low bytes (little-endian).
          auto fetch = type_of(cxx(f_.results_of(n)[1]));
          if (!fetch) return fail(fetch.error());
          ValueId state = in(n, 0), cursor = in(n, 1);
          ValueId at = emit1(Op2::load, T::ptr, {state, cursor}, {0});
          ValueId value = emit1(Op2::load, *fetch, {state, at}, {0});
          T st[] = {T::state};
          ValueId ops[] = {state, cursor, add_offset(at, 8)};
          std::uint64_t im[] = {0};
          out(n, 0, b_.peek().result(b_.emit(Op2::store, st, ops, im, 0, {}, o)));
          out(n, 1, value);
          return {};
        }
        ValueId ops[] = {in(n, 0), in(n, 1)};
        if (nd.op == Op::va_start) {
          T result[] = {T::state};
          out(n, 0, b_.peek().result(b_.emit(Op2::va_start, result, ops, {}, 0, {}, o)));
        } else {
          auto fetch = type_of(cxx(f_.results_of(n)[1]));
          if (!fetch) return fail(fetch.error());
          T results[] = {T::state, *fetch};
          auto m = b_.emit(Op2::va_arg, results, ops, {}, 0, {}, o);
          out(n, 0, b_.peek().result(m));
          out(n, 1, b_.peek().result(m, 1));
        }
        return {};
      }
      case Op::va_copy: {
        QualType cursor = layout(cxx(f_.operands_of(n)[1])).pointee;
        out(n, 0, copy_bytes(in(n, 0), in(n, 1), in(n, 2), layout(cursor), 0));
        return {};
      }
      case Op::va_end:
        // The operand has been evaluated. The ABI requires no cleanup of the cursor.
        out(n, 0, in(n, 0));
        return {};
      case Op::bit_field_load:
      case Op::bit_field_store: {
        QualType owner = layout(cxx(f_.operands_of(n)[1])).pointee;
        SubObject field = layout_.step(owner, {Step::field, std::uint32_t(imms[0])});
        if (!field.bit_field) return fail("no bit-field layout for the projected member");
        if (nd.op == Op::bit_field_load) out(n, 0, load_field(in(n, 0), in(n, 1), field));
        else out(n, 0, store_field(in(n, 0), in(n, 1), field, in(n, 2)));
        return {};
      }
      case Op::call:
      case Op::call_ind: {
        bool ind = nd.op == Op::call_ind;
        std::vector<ValueId> ops{in(n, 0)};
        auto l1ops = f_.operands_of(n);
        if (ind) ops.push_back(in(n, 1));
        Amd64ArgumentOrder amd64_args;
        std::uint64_t sext = 0;
        std::uint64_t byval = imms[ind ? 1 : 2], result_arg = imms[ind ? 2 : 3];
        bool place_amd64 = config_.sysv_amd64 && byval;
        std::uint32_t indirect = 0;           // L2's: 1 + the index of the indirect result's address
        std::optional<ValueId> result_dst;    // a class result in registers: where it goes
        ClassPieces result_pieces;
        ValueId state = in(n, 0);
        // System V AMD64: a class result in memory goes first, as in `block`.
        std::optional<std::size_t> sysv_result;
        if (config_.sysv_amd64 && result_arg) {
          const std::size_t i = (ind ? 2 : 1) + result_arg - 1;
          if (i < l1ops.size() && sysv_memory_result(layout(cxx(l1ops[i])).pointee)) {
            sysv_result = result_arg - 1;
            if (place_amd64) amd64_args.add(std::uint32_t(ops.size()), T::ptr);
            ops.push_back(map_[raw(l1ops[i])]);
            indirect = 1;
          }
        }
        for (std::size_t i = ind ? 2 : 1, k = 0; i < l1ops.size(); ++i, ++k) {
          if (sysv_result && k == *sysv_result) continue;
          ValueId v = map_[raw(l1ops[i])];
          if (k >= 32) return fail("more than 32 arguments are not in the α subset");
          std::size_t at = ops.size() - (ind ? 2 : 1);  // the argument's L2 index
          const bool class_arg = (byval >> k) & 1, class_result = k + 1 == result_arg;
          QualType c{};
          if (class_arg || class_result) {
            c = layout(cxx(l1ops[i])).pointee;
            const TypeLayout object = layout(c);
            if (object.copy_abi_unknown) return fail("defaulted copy ABI classification is unavailable");
            if (!config_.sysv_amd64 && object.contains_explicit_alignment)
              return fail("AArch64 Darwin ABI: explicitly aligned classes by value are not supported yet");
            // An empty class passes nothing, and returns nothing.
            if (ignored(c)) continue;
          }
          if (class_result) {
            result_pieces = class_pieces(c);
            if (result_pieces.words) {
              result_dst = v;
              continue;
            }
            if (config_.sysv_amd64) return fail("unsupported System V AMD64 aggregate result");
            indirect = std::uint32_t(at + 1);
          } else if (class_arg) {
            // A class argument represented by pieces of its caller-owned object.
            ClassPieces pieces = class_pieces(c, &amd64_args, std::uint32_t(ops.size()));
            if (config_.sysv_amd64 && !pieces.words && !layout(c).trivial_calls) {
              // By the address of the caller's object (Itanium C++ ABI §3.1.2.3), below.
            } else if (pieces.words) {
              if (!config_.sysv_amd64) {
                const T piece = pieces.type;
                std::span<const ValueId> args(ops.begin() + (ind ? 2 : 1), ops.end());
                pad_registers(used(args, piece, 0, indirect), pieces.words, piece,
                              [&] { ops.push_back(constant(piece, 0)); });
              }
              for (std::uint32_t w = 0; w < pieces.words; ++w)
                ops.push_back(load_class_piece(state, v, pieces, w));
              continue;
            }
            else if (config_.sysv_amd64) return fail("unsupported System V AMD64 aggregate argument");
          }
          bool signed_arg = b_.peek().type(v) != T::i1 && b_.peek().type(v) != T::ptr && is_signed(l1ops[i]);
          if (place_amd64) {
            auto location = amd64_args.add(std::uint32_t(ops.size()), b_.peek().type(v));
            if (location.kind == Amd64ArgumentKind::integer && signed_arg)
              sext |= std::uint64_t{1} << location.index;
          } else if (at < 32 && signed_arg) {
            sext |= std::uint64_t{1} << at;
          }
          ops.push_back(v);
        }
        if (place_amd64) {
          std::vector<ValueId> ordered(ops.begin(), ops.begin() + (ind ? 2 : 1));
          for (std::uint32_t i : std::move(amd64_args).order())
            ordered.push_back(i == none ? constant(T::i64, 0) : ops[i]);
          ops = std::move(ordered);
        } else if (ops.size() - (ind ? 2 : 1) > 32) {
          return fail("more than 32 argument registers are not in the α subset");
        }
        std::vector<T> res{T::state};
        res.insert(res.end(), result_pieces.words, result_pieces.type);
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
        im.push_back(indirect);
        Flags flags = nd.flags & bit(Flag::variadic);
        NodeId c = b_.emit(ind ? Op2::call_ind : Op2::call, res, ops, im, flags, {}, o);
        ValueId after = b_.peek().result(c);
        if (result_dst) {
          for (std::uint32_t w = 0; w < result_pieces.words; ++w)
            after = store_class_piece(after, *result_dst, result_pieces, w, b_.peek().result(c, 1 + w), true);
          out(n, 0, after);
          return {};
        }
        out(n, 0, after);
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
      case Op::invoke: {
        if (config_.cleanup_resume != EntityId::none && !catch_edges_ &&
            nd.nopnd == 1 && nd.nres == 1 && imms[1] == 0) {
          const auto edges = f_.succs_of(n);
          if (edges.size() != 2 || edges[0].nargs || edges[1].nargs) return fail("unsupported hosted cleanup successor");
          const BlockId landing{block_map_[raw(edges[1].target)]};
          catch_edges_ = CatchEdges{n, edges[0].target, edges[1].target, landing, {}, true};
          const l2::Builder::SuccArgs paths[] = {{BlockId(block_map_[raw(edges[0].target)]), {}}, {landing, {}}};
          const ValueId args[] = {in(n, 0)};
          const std::uint64_t target[] = {imms[0], 0, 0, 0};
          b_.emit(Op2::invoke_cleanup, {}, args, target, 0, paths, origin());
          return {};
        }
        if (!config_.sysv_amd64 || !config_.catch_all || catch_edges_ || nd.nopnd != 2 || nd.nres != 2 ||
            imms[1] != 0 || b_.peek().type(in(n, 1)) != T::i32)
          return fail("unsupported hosted invoke");
        const auto edges = f_.succs_of(n);
        // The invoke's first result is state. Its second must be the fixed int result.
        const auto type = type_of(cxx(l1_result(n, 1)));
        if (!type || *type != T::i32 || edges.size() != 2 || edges[0].nargs || edges[1].nargs)
          return fail("unsupported hosted invoke result or successor");
        const auto next = static_cast<std::uint32_t>(std::ranges::count_if(block_map_, [](auto b) { return b != none; }));
        catch_edges_ = CatchEdges{n, edges[0].target, edges[1].target, BlockId(next), BlockId(next + 1)};
        const l2::Builder::SuccArgs paths[] = {
          {BlockId(block_map_[raw(edges[0].target)]), {}}, {catch_edges_->landing, {}},
        };
        const ValueId args[] = {in(n, 0), in(n, 1)};
        const std::uint64_t target[] = {imms[0], 1, 0, 0};
        b_.emit(Op2::invoke, {}, args, target, 0, paths, origin());
        return {};
      }
      case Op::resume: {
        if (!catch_edges_ || !catch_edges_->cleanup || nd.block != raw(catch_edges_->handler))
          return fail("unsupported hosted resume");
        const ValueId args[] = {in(n, 0), in(n, 1)};
        const std::uint64_t target[] = {raw(config_.cleanup_resume), 0, 0, 0};
        const T result[] = {T::state};
        const auto o = origin();
        b_.emit(Op2::call, result, args, target, 0, {}, o);
        b_.emit(Op2::unreachable, {}, {}, {}, 0, {}, o);
        return {};
      }
      case Op::catch__begin:
      case Op::catch__end: {
        if (!catch_edges_ || nd.block != raw(catch_edges_->handler))
          return fail("unsupported hosted catch operation");
        const bool begin = nd.op == Op::catch__begin;
        const T results[] = {T::state, T::ptr};
        const ValueId args[] = {in(n, 0), in(n, 1)};
        const std::uint64_t target[] = {raw(begin ? config_.catch_all->begin : config_.catch_all->end), 0, 0, 0};
        const std::size_t count = begin ? 2 : 1;
        const auto call = b_.emit(Op2::call, std::span(results).first(count), std::span(args).first(count),
                                  target, 0, {}, origin());
        for (std::uint32_t i = 0; i < count; ++i) out(n, i, b_.peek().result(call, i));
        return {};
      }
      case Op::ret: {
        std::vector<ValueId> ops{in(n, 0)};
        std::vector<T> types;
        auto l1ops = f_.operands_of(n);
        // A class result in registers: its object's words or HFA members.
        for (std::uint32_t w = 0; w < result_pieces_.words; ++w) {
          ops.push_back(load_class_piece(in(n, 0), *result_object_, result_pieces_, w));
          types.push_back(result_pieces_.type);
        }
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
      case Op::throw_: {
        if (!config_.sysv_amd64 || !config_.int_throw || nd.nsucc != 0 || imms[0] != raw(config_.int_throw->type) ||
            b_.peek().type(in(n, 1)) != T::i32)
          return fail("`throw` is not in the α subset");
        const auto& abi = *config_.int_throw;
        const auto o = origin();
        // The L1 operand has already been evaluated. Its call, if any, can unwind without
        // leaving an allocated exception object behind (Itanium EH ABI §2.4).
        const T allocation_results[] = {T::state, T::ptr};
        const ValueId allocation_args[] = {in(n, 0), constant(T::i64, l2::bytes(T::i32))};
        const std::uint64_t allocation_call[] = {raw(abi.allocate), 0, 0, 0};
        const NodeId allocation = b_.emit(Op2::call, allocation_results, allocation_args, allocation_call, 0, {}, o);
        const ValueId payload = b_.peek().result(allocation, 1);
        const T state_result[] = {T::state};
        const ValueId store_args[] = {b_.peek().result(allocation), payload, in(n, 1)};
        const std::uint64_t alias[] = {imms[0]};
        const NodeId store = b_.emit(Op2::store, state_result, store_args, alias, 0, {}, o);
        const ValueId type_info = emit1(Op2::symbol, T::ptr, {}, {raw(abi.type_info)});
        const ValueId throw_args[] = {b_.peek().result(store), payload, type_info, constant(T::ptr, 0)};
        const std::uint64_t throw_call[] = {raw(abi.throw_), 0, 0, 0};
        b_.emit(Op2::call, state_result, throw_args, throw_call, 0, {}, o);
        b_.emit(Op2::unreachable, {}, {}, {}, 0, {}, o);
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

  // AAPCS64 C.3, C.11: when `n` registers of `piece`'s class are wanted after `used` and fewer are
  // left, those left are skipped, `add` filling each with a value no one reads.
  template <class Add>
  void pad_registers(std::size_t used, std::uint32_t n, T piece, Add&& add) const {
    std::uint32_t regs = l2::is_float(piece) ? config_.float_arg_registers : config_.int_arg_registers;
    if (used < regs && used + n > regs)
      for (std::size_t r = used; r < regs; ++r) add();
  }
  // How many of the values in `vals`, from index `first`, take a register of `piece`'s class: the
  // floating ones, or the others but the indirect result's address (`indirect`, 1 + its index).
  template <class Vals>
  std::size_t used(const Vals& vals, T piece, std::size_t first, std::uint32_t indirect) const {
    std::size_t n = 0;
    for (std::size_t i = first; i < vals.size(); ++i) {
      T t;
      if constexpr (std::same_as<std::ranges::range_value_t<Vals>, T>) t = vals[i];
      else t = b_.peek().type(vals[i]);
      if (t == T::state || i - first + 1 == indirect) continue;
      n += l2::is_float(t) == l2::is_float(piece);
    }
    return n;
  }
  // An AMD64 record's INTEGER eightbytes, or its full stack representation. On AAPCS64
  // a small record uses integer words or, for an HFA, one floating register per member.
  ClassPieces class_pieces(QualType c, Amd64ArgumentOrder* args = nullptr, std::uint32_t first = 0) const {
    TypeLayout l = layout(c);
    if (l.kind != TypeKind::record || !l.trivial_calls) return {};
    auto words = std::uint32_t((l.size + 7) / 8);
    if (config_.sysv_amd64) {
      std::uint8_t chunks = l.sysv_integer_eightbytes;
      if (!chunks) return {};
      auto registers = std::uint32_t(std::popcount(chunks));
      if (!args) return {registers, T::i64, l.size, chunks};
      bool in_registers = args->add(first, T::i64, registers, words).kind == Amd64ArgumentKind::integer;
      if (!in_registers) chunks = std::uint8_t((1u << words) - 1);
      std::uint32_t passed_words = in_registers ? registers : words;
      return {passed_words, T::i64, l.size, chunks};
    }
    if (l.hfa) return {l.hfa, l.hfa_bytes == 4 ? T::f32 : T::f64, l.size};
    if (l.size > config_.composite_in_registers) return {};
    return {words, T::i64, l.size};
  }
  // System V AMD64: a class result the caller provides memory for, whose address it passes
  // as the first INTEGER argument and the callee returns in RAX. A class not trivial for calls
  // (Itanium C++ ABI §3.1.3.1), or one of more than two eightbytes (psABI §3.2.3, MEMORY).
  bool sysv_memory_result(QualType c) const {
    TypeLayout l = layout(c);
    return l.kind == TypeKind::record && !l.empty && !l.copy_abi_unknown && !l.layout_uncertain &&
           ((!l.trivial_calls && !l.calls_abi_uncertain) || l.size > 16);
  }
  // An empty class trivially copyable for calls is not passed at all, as Clang does on arm64
  // macOS (#116).
  bool ignored(QualType c) const {
    TypeLayout l = layout(c);
    return !config_.sysv_amd64 && l.kind == TypeKind::record && l.trivial_calls && l.empty;
  }
  // An ABI-local class object. AAPCS64 rounds it up to permit full-word accesses.
  ValueId frame_object(const TypeLayout& l) {
    std::uint64_t size = config_.sysv_amd64 ? l.size : (l.size + 7) / 8 * 8;
    std::uint32_t align = config_.sysv_amd64 ? l.align : std::max<std::uint32_t>(l.align, 8);
    return emit1(Op2::alloca, T::ptr, {}, {size, align, 0x80000000u + frame_objects_++});
  }
  // Pack a possibly partial AMD64 eightbyte, keeping every access within the object.
  ValueId load_class_piece(ValueId s, ValueId obj, const ClassPieces& pieces, std::uint32_t i) {
    const auto offset = pieces.offset(i);
    if (!config_.sysv_amd64 || pieces.width(i) == 8)
      return emit1(Op2::load, pieces.type, {s, add_offset(obj, offset)}, {0});
    ValueId word{};
    const auto size = pieces.width(i);
    for (std::uint64_t done = 0; done < size;) {
      const auto width = std::bit_floor(size - done);
      T t = *l2::int_of_bits(width * 8);
      ValueId piece = emit1(Op2::load, t, {s, add_offset(obj, offset + done)}, {0});
      piece = resize(piece, T::i64, false);
      if (!done) word = piece;
      else {
        piece = emit1(Op2::shl, T::i64, {piece, constant(T::i64, 8 * done)});
        word = emit1(Op2::or_, T::i64, {word, piece});
      }
      done += width;
    }
    return word;
  }
  ValueId store_class_piece(ValueId s, ValueId obj, const ClassPieces& pieces, std::uint32_t i, ValueId v,
                            bool bounded) {
    const auto offset = pieces.offset(i);
    if (pieces.type == T::i64) return store_piece(s, obj, offset, bounded ? pieces.width(i) : 8, v);
    auto o = origin();
    T st[] = {T::state};
    ValueId ops[] = {s, add_offset(obj, offset), v};
    std::uint64_t im[] = {0};
    return b_.peek().result(b_.emit(Op2::store, st, ops, im, 0, {}, o));
  }
  // Stores the low `size` bytes of the 64-bit word `v` at `p + off`, little-endian, in pieces
  // that do not write past them.
  ValueId store_piece(ValueId s, ValueId p, std::uint64_t off, std::uint64_t size, ValueId v) {
    std::uint64_t done = 0;
    while (done < size) {
      std::uint64_t w = 8;
      while (w > size - done) w /= 2;
      ValueId piece = done ? emit1(Op2::lshr, T::i64, {v, constant(T::i64, std::int64_t(8 * done))}) : v;
      T t = *l2::int_of_bits(w * 8);
      if (t != T::i64) piece = emit1(Op2::trunc, t, {piece});
      auto o = origin();
      T st[] = {T::state};
      ValueId ops[] = {s, add_offset(p, off + done), piece};
      std::uint64_t im[] = {0};
      s = b_.peek().result(b_.emit(Op2::store, st, ops, im, 0, {}, o));
      done += w;
    }
    return s;
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
