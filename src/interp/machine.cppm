// The step machine of the L1 interpreter (optimization design §2.3; DECISIONS §4.6): abstract
// memory and one handler per op, gathered by `ir::eval_table`.
//
// Memory is abstract ([basic.memobj]): an object is a type and a run of cells, one per scalar
// leaf in declaration order (an array's elements in order). Each cell holds its lifetime state
// (before, alive, after: [basic.life]; the initializing store comes before `obj.begin`), what it
// holds (an indeterminate value, an erroneous one, or a value: [basic.indet]) and the value. A
// pointer is `(object, access path)` with provenance: it never becomes an integer, and arithmetic
// moves it only within its array. A pointer to a non-array object is treated as one to an array
// of one element ([expr.add]), with a flag for one past it.
//
// The machine is trampolined: calls push a frame on its own stack, never the C++ one. It stops
// whenever it needs something from outside (a callee's body, a global variable) or every
// `poll_interval` steps, and the driver (`evaluate`, interp.cppm) fetches it or polls for
// cancellation, then resumes it. It counts steps, one per node, and nested calls, against the
// limits of its `Config` ([implimits]). The same machine runs in `constant` mode, where
// [expr.const] holds, and in `runtime` mode, for whole programs.
//
// Bodies must pass `ir::verify`. Within that, nothing in a body crashes the machine: an operation
// the α slice does not evaluate yet, or a value of the wrong kind, is a failure with a reason.
export module ycxx.interp:machine;

import std;
import ycxx.ast;
import ycxx.ir;
import :reason;

export namespace ycxx::interp {

using ir::ConstId;
using ir::EntityId;
using ir::NodeId;
using ir::PathStep;
using ir::QualType;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

enum class Mode : u8 { constant, runtime };

struct Config {
  Mode mode = Mode::constant;
  u64 step_limit = u64{1} << 22;  // nodes executed
  u32 depth_limit = 512;          // nested calls ([implimits]: recursive constexpr calls)
  u32 poll_interval = 4096;       // steps between cancellation polls; 0: never
};

// A value in a register or a cell. An integer (bool and the character types included) is held
// normalized to its type: sign-extended from its width when signed, zero-extended otherwise. A
// floating value is its encoding in its type's format, binary32 in the low 32 bits.
enum class VKind : u8 { none, integer, null, object, function, floating };
struct Value {
  VKind kind = VKind::none;
  bool past = false;  // object: one past the end of a non-array object
  u32 ref = 0;        // object: the object's index; function: the EntityId
  u64 bits = 0;       // integer: the value; object: the access path's id

  static constexpr Value integer(u64 bits) { return {VKind::integer, false, 0, bits}; }
  static constexpr Value floating(u64 bits) { return {VKind::floating, false, 0, bits}; }
  static constexpr Value null() { return {VKind::null}; }
  static constexpr Value function(EntityId e) { return {VKind::function, false, std::to_underlying(e), 0}; }
  constexpr bool operator==(const Value&) const = default;
};

// A namespace-scope variable, as the host answers for it (query-engine.md §B7: in the engine,
// `constant_value(V, ctx)` for one usable in constant expressions).
struct GlobalVar {
  QualType type{};                // as declared, cv included
  std::optional<ConstId> value;   // its value when constant-initialized; else zero
  bool usable = false;            // usable in constant expressions ([expr.const])
};

// A callee, as the host answers for it (in the engine, a fetch of `function_ir(F, ctx)`).
struct Callee {
  const ir::Function* body = nullptr;  // none: not defined ([expr.const]: before the evaluation)
  bool is_constexpr = false;           // a constexpr or consteval function ([dcl.constexpr])
  bool unsupported = false;            // defined, but its body has no L1 yet
};

// Where an evaluation was: the function and the node, innermost first. The caller maps each to a
// position through that function's `function_debug`.
struct Site {
  EntityId function{};
  NodeId node{};
  constexpr bool operator==(const Site&) const = default;
};

struct Failure {
  Reason reason = Reason::ill_formed;
  u64 arg = 0;  // the message's `{0}`
  std::vector<Site> stack;
};

// A result, described without the machine's object numbers.
enum class Base : u8 { global, literal, local };
struct Result {
  enum class Kind : u8 { integer, null, address, function, floating };
  Kind kind = Kind::integer;
  QualType type{};
  u64 bits = 0;              // integer, normalized to `type`; floating, its encoding
  Base base = Base::local;   // address
  EntityId entity{};         // address of a global; function
  ConstId literal{};         // address within a string literal object
  std::vector<PathStep> path;
  bool past = false;
  i64 offset = 0;            // address: in bytes from the start of the object
  bool operator==(const Result&) const = default;
};

enum class Event : u8 { done, failed, need_body, need_global, poll };

struct Ops;

class Machine {
public:
  Machine(const ast::Interners& in, const ast::Target& target, Config config)
      : in_(&in), target_(&target), config_(config) {
    paths_.push_back({0, {}});
  }
  Machine(const Machine&) = delete;

  // Starts the evaluation of `entry`, named `name` in sites, with its parameters' values.
  void start(const ir::Function& entry, EntityId name, std::span<const Value> args);
  // Runs until the evaluation ends or needs something; `need_*` names it in `wanted()`.
  Event run();
  EntityId wanted() const { return wanted_; }
  // Answer `need_body` and `need_global`.
  void give_body(EntityId e, Callee c) { bodies_[std::to_underlying(e)] = c; }
  void give_global(EntityId e, std::optional<GlobalVar> g);

  const Failure& failure() const { return failure_; }
  u64 steps() const { return steps_; }
  // After `done`: the entry's results, described.
  std::vector<Result> results() const;

private:
  friend struct Ops;

  enum class Flow : u8 { next, jumped, called, returned, suspend, stop };
  enum class Life : u8 { before, alive, after };
  // `constant`: a global's initial value, the ConstId in `v.bits`, decoded when first read.
  enum class Fill : u8 { indeterminate, erroneous, value, constant };
  enum class Origin : u8 { automatic, global, literal };
  enum class Use : u8 { read, write, lifetime };
  enum class Kind : u8 { integer, pointer, array, function, floating, other };

  struct Cell {
    Life life = Life::before;
    Fill fill = Fill::indeterminate;
    Value v{};
  };
  struct Object {
    QualType type{};
    Origin origin = Origin::automatic;
    bool storage = true;    // false once deallocated
    bool outside = false;   // its lifetime began outside the evaluation
    bool usable = true;     // readable in constant mode
    bool read_only = false; // a string literal object
    EntityId entity{};
    ConstId literal{};
    u32 cell0 = 0, ncells = 0;
  };
  struct PathNode {
    u32 parent = 0;
    PathStep step{};
  };
  struct Shape {
    Kind kind = Kind::other;
    u32 width = 0;  // integer; floating: 32 (binary32) or 64 (binary64)
    bool is_signed = false;
    QualType inner{};  // pointee or element; none for std::nullptr_t
    bool is_nullptr = false;
    u64 bound = 0;     // array
    u64 leaves = 0;    // cells of an object of this type
    u64 size = 0;      // bytes ([basic.types.general]); 0 for a function type
    bool is_const = false;
  };
  struct Frame {
    const ir::Function* fn = nullptr;
    EntityId entity{};
    std::vector<Value> regs;
    u32 node = 0;
  };
  // A designated subobject: its object, its cells and its type.
  struct Place {
    u32 obj = 0;
    u32 cell0 = 0, ncells = 0;
    QualType type{};
  };
  // An unsequenced group ([intro.execution]): the accesses its operands made so far.
  struct Access {
    u32 obj, cell0, ncells, segment;
    bool write;
  };
  struct Group {
    std::size_t frame;
    u32 segment = 0;
    std::vector<Access> accesses;
  };

  // ------------------------------------------------------------ types

  const Shape& shape(QualType q);
  bool similar(QualType a, QualType b);
  bool accessible_as(QualType object, QualType glvalue);

  // ------------------------------------------------------------ paths

  u32 child(u32 path, PathStep s);
  std::vector<PathStep> steps_of(u32 path) const;
  u32 extend(u32 path, std::span<const u64> words);

  // ------------------------------------------------------------ objects

  u32 make_object(QualType t, Origin o);
  void fill_zero(QualType t, u32 cell);
  bool fill_array_constant(QualType t, u32 cell, ConstId value);
  std::optional<Place> resolve(const Value& p, std::span<const u64> extra, bool& past);
  Flow locate(const Value& p, std::span<const u64> extra, QualType t, Use use, bool volatile_, Place& out);
  Flow note_access(const Place& p, bool write);
  Flow constant(ConstId c, QualType type, Value& out);
  std::optional<u32> literal_object(ConstId s);
  Flow global_object(EntityId e, u32& out);
  Flow address(u32 obj, QualType pointee, i64 offset, Value& out);
  Flow address_from_path(u32 obj, QualType pointee, std::span<const u32> indices, i64 offset, Value& out);
  i64 offset_of(const Value& p);
  bool may_overlap(const Value& a, const Value& b);

  // ------------------------------------------------------------ execution

  Flow fail(Reason r, u64 arg = 0);
  Flow unsupported(NodeId n) { return fail(Reason::unsupported, std::to_underlying(frames_.back().fn->node(n).op)); }
  Value& reg(Frame& f, ir::ValueId v) { return f.regs[ir::raw(v)]; }
  Value operand(Frame& f, NodeId n, u32 i) { return f.regs[ir::raw(f.fn->operands_of(n)[i])]; }
  QualType type_of(Frame& f, ir::ValueId v) const { return f.fn->value(v).type.type; }
  QualType result_type(Frame& f, NodeId n) const { return type_of(f, *f.fn->results_of(n).begin()); }
  Flow set(Frame& f, NodeId n, Value v, u32 i = 0) {
    f.regs[ir::raw(f.fn->results_of(n)[i])] = v;
    return Flow::next;
  }
  Flow jump(Frame& f, NodeId n, u32 succ);
  Flow enter(Frame& f, NodeId n, const ir::Function* body, EntityId callee, std::span<const ir::ValueId> args);

  // Integer helpers: `x` normalized to an integer type of `width` bits.
  static u64 norm(u64 x, u32 width, bool is_signed);
  static bool fits(i64 x, u32 width) { return width >= 64 || (x >= -(i64{1} << (width - 1)) && x < (i64{1} << (width - 1))); }
  static bool fits_unsigned(u64 x, u32 width) { return width >= 64 || x < (u64{1} << width); }
  Flow int_shape(Frame& f, QualType t, const Shape*& s);
  Flow arith(Frame& f, NodeId n);
  Flow divide(Frame& f, NodeId n);
  Flow shift(Frame& f, NodeId n);
  Flow compare_pointers(Frame& f, NodeId n, ir::Pred pred, const Value& a, const Value& b);

  // Floating-point helpers: the shape of a floating type, the operations and the conversions.
  Flow float_shape(QualType t, const Shape*& s);
  Flow float_arith(Frame& f, NodeId n);
  Flow float_compare(Frame& f, NodeId n);
  Flow float_convert(Frame& f, NodeId n);
  Flow pointer_index(const Value& p, QualType elem, u32& base_path, i64& index, u64& bound, bool& single);

  const ast::Interners* in_;
  const ast::Target* target_;
  Config config_;

  std::vector<Frame> frames_;
  std::vector<Object> objects_;
  std::vector<Cell> cells_;
  std::vector<PathNode> paths_;
  std::map<std::pair<u32, u64>, u32> path_index_;
  std::map<u32, Shape> shapes_;
  std::map<u32, Callee> bodies_;
  std::map<u32, std::optional<GlobalVar>> global_info_;
  std::map<u32, u32> globals_;
  std::vector<Group> groups_;

  EntityId wanted_{};
  Event pending_ = Event::done;
  bool failed_ = false;
  Failure failure_;
  u64 steps_ = 0;
  u32 since_poll_ = 0;
  std::vector<Value> results_;
  std::vector<QualType> result_types_;
};

}  // namespace ycxx::interp

namespace ycxx::interp {

using ir::Op;

// ------------------------------------------------------------------ types

const Machine::Shape& Machine::shape(QualType q) {
  QualType c = in_->canonical(q);
  u32 key = std::to_underlying(c);
  if (auto it = shapes_.find(key); it != shapes_.end()) return it->second;
  Shape s;
  s.is_const = (std::to_underlying(ast::cv_of(c)) & 1) != 0;
  if (auto b = ast::builtin_of(*in_, c)) {
    if (ast::is_integral(*b) && ast::width_of(*target_, *b) <= 64) {
      s.kind = Kind::integer;
      s.width = ast::width_of(*target_, *b);
      s.is_signed = ast::is_signed(*target_, *b);
      s.leaves = 1;
      s.size = (*target_)[*b].size;
    } else if (ast::is_floating(*b) && ast::computes((*target_)[*b].format)) {
      s.kind = Kind::floating;
      s.width = (*target_)[*b].format == ast::FloatFormat::binary32 ? 32 : 64;
      s.leaves = 1;
      s.size = (*target_)[*b].size;
    } else if (*b == ast::Builtin::nullptr_t) {
      s.kind = Kind::pointer;
      s.is_nullptr = true;
      s.leaves = 1;
      s.size = (*target_)[*b].size;
    }
  } else if (in_->types.is<ast::TPointer>(c)) {
    s.kind = Kind::pointer;
    s.inner = in_->types.get<ast::TPointer>(c).pointee;
    s.leaves = 1;
    s.size = target_->pointer_size;
  } else if (in_->types.is<ast::TArray>(c)) {
    auto a = in_->types.get<ast::TArray>(c);
    s.kind = Kind::array;
    s.inner = a.elem;
    s.bound = a.bound == ConstId::none ? 0 : in_->values.get<ast::VInt>(a.bound).bits.lo;
    // An array's elements carry its cv ([basic.type.qualifier]); `canonical` pushed it there.
    Shape e = shape(a.elem);
    s.is_const = e.is_const;
    s.leaves = e.leaves * s.bound;
    s.size = e.size * s.bound;
  } else if (in_->types.is<ast::TFunction>(c)) {
    s.kind = Kind::function;
  }
  return shapes_.emplace(key, s).first->second;
}

// Similar types ([conv.qual]): the same up to cv-qualification at every level.
bool Machine::similar(QualType a, QualType b) {
  QualType ca = ast::without_cv(in_->canonical(a)), cb = ast::without_cv(in_->canonical(b));
  if (ca == cb) return true;
  const Shape sa = shape(ca), sb = shape(cb);
  if (sa.kind != sb.kind) return false;
  if (sa.kind == Kind::pointer) return !sa.is_nullptr && !sb.is_nullptr && similar(sa.inner, sb.inner);
  if (sa.kind == Kind::array) return sa.bound == sb.bound && similar(sa.inner, sb.inner);
  return false;
}

// [basic.lval]/11: an object is accessed through a glvalue of a similar type or, outside constant
// evaluation, of its signed or unsigned counterpart.
bool Machine::accessible_as(QualType object, QualType glvalue) {
  if (similar(object, glvalue)) return true;
  if (config_.mode == Mode::constant) return false;
  auto a = ast::builtin_of(*in_, object), b = ast::builtin_of(*in_, glvalue);
  if (!a || !b || *a == ast::Builtin::bool_ || *b == ast::Builtin::bool_) return false;
  if (!ast::is_integral(*a) || !ast::is_integral(*b)) return false;
  auto standard = [](ast::Builtin x) { return ast::is_standard_signed(x) || ast::is_standard_unsigned(x); };
  if (!standard(*a) || !standard(*b)) return false;
  return ast::unsigned_counterpart(*a) == ast::unsigned_counterpart(*b);
}

// ------------------------------------------------------------------ paths

// Paths are interned in a trie: id 0 is the empty path, and each node is its parent plus a step.
u32 Machine::child(u32 path, PathStep s) {
  auto key = std::pair(path, ir::encode(s));
  if (auto it = path_index_.find(key); it != path_index_.end()) return it->second;
  u32 id = u32(paths_.size());
  paths_.push_back({path, s});
  path_index_.emplace(key, id);
  return id;
}

std::vector<PathStep> Machine::steps_of(u32 path) const {
  std::vector<PathStep> r;
  for (; path != 0; path = paths_[path].parent) r.push_back(paths_[path].step);
  std::ranges::reverse(r);
  return r;
}

// `path` followed by an immediate access path: its length, then its encoded steps.
u32 Machine::extend(u32 path, std::span<const u64> words) {
  for (u64 w : words) path = child(path, ir::decode_step(w));
  return path;
}

// ------------------------------------------------------------------ objects

u32 Machine::make_object(QualType t, Origin o) {
  Object ob;
  ob.type = in_->canonical(t);
  ob.origin = o;
  ob.cell0 = u32(cells_.size());
  ob.ncells = u32(shape(t).leaves);
  cells_.resize(cells_.size() + ob.ncells);
  objects_.push_back(ob);
  return u32(objects_.size() - 1);
}

void Machine::fill_zero(QualType t, u32 cell) {
  const Shape s = shape(t);
  if (s.kind == Kind::array) {
    u64 n = shape(s.inner).leaves;
    for (u64 i = 0; i < s.bound; ++i) fill_zero(s.inner, u32(cell + i * n));
    return;
  }
  cells_[cell].fill = Fill::value;
  cells_[cell].v = s.kind == Kind::pointer ? Value::null() : s.kind == Kind::floating ? Value::floating(0) : Value::integer(0);
}

// [dcl.init.aggr]: each nested array supplies a value for every element. Leave pointer and
// other scalar constants to the usual lazy load, which can ask the host for another global.
bool Machine::fill_array_constant(QualType t, u32 cell, ConstId value) {
  const ast::Values& vs = in_->values;
  const Shape& s = shape(t);
  switch (s.kind) {
  case Kind::array: {
    if (!vs.is<ast::VAggregate>(value)) return false;
    auto aggregate = vs.get<ast::VAggregate>(value);
    auto elements = in_->seqs.elements(aggregate.elems);
    if (!similar(t, aggregate.type) || elements.size() != s.bound) return false;
    u32 leaves = u32(shape(s.inner).leaves);
    for (ConstId element : elements) {
      if (!fill_array_constant(s.inner, cell, element)) return false;
      cell += leaves;
    }
    return true;
  }
  case Kind::integer:
    if (!vs.is<ast::VInt>(value) || !similar(t, vs.get<ast::VInt>(value).type)) return false;
    break;
  case Kind::floating:
    if (!vs.is<ast::VFloat>(value) || !similar(t, vs.get<ast::VFloat>(value).type)) return false;
    break;
  case Kind::pointer:
    if (!vs.is<ast::VNullptr>(value) && (s.is_nullptr || !vs.is<ast::VLvalue>(value))) return false;
    break;
  default:
    return false;
  }
  cells_[cell] = {Life::alive, Fill::constant, {VKind::none, false, 0, std::to_underlying(value)}};
  return true;
}

// The subobject `p` designates, followed by `extra`. `past` is set when it is one past the end
// of an object; then the place is the object it is past. None when the path does not fit the
// object's type.
std::optional<Machine::Place> Machine::resolve(const Value& p, std::span<const u64> extra, bool& past) {
  const Object& ob = objects_[p.ref];
  Place pl{p.ref, ob.cell0, ob.ncells, ob.type};
  past = p.past;
  std::vector<PathStep> path = steps_of(u32(p.bits));
  for (u64 w : extra) path.push_back(ir::decode_step(w));
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (past) return std::nullopt;  // a step below a past-the-end place
    const Shape& s = shape(pl.type);
    if (path[i].step != ir::Step::elem || s.kind != Kind::array) return std::nullopt;
    if (path[i].index > s.bound) return std::nullopt;
    u64 n = shape(s.inner).leaves;
    pl.type = s.inner;
    if (path[i].index == s.bound) {
      past = true;
      pl.ncells = 0;
      pl.cell0 = u32(pl.cell0 + s.bound * n);
      continue;
    }
    pl.cell0 = u32(pl.cell0 + path[i].index * n);
    pl.ncells = u32(n);
  }
  return pl;
}

// Checks an access of a `t` through pointer `p` (and `extra`): [basic.life], [basic.lval],
// [dcl.type.cv] and, in constant mode, [expr.const]. On success `out` is the place.
Machine::Flow Machine::locate(const Value& p, std::span<const u64> extra, QualType t, Use use, bool volatile_,
                              Place& out) {
  if (p.kind == VKind::null) return fail(Reason::null_access);
  if (p.kind != VKind::object) return fail(p.kind == VKind::function ? Reason::access_type : Reason::ill_formed);
  if (volatile_ && config_.mode == Mode::constant) return fail(Reason::volatile_access);
  const Object& ob = objects_[p.ref];
  if (!ob.storage) return fail(Reason::storage_ended);
  bool past = false;
  auto pl = resolve(p, extra, past);
  if (!pl) return fail(Reason::past_end_access);
  if (past) return fail(Reason::past_end_access);
  if (!accessible_as(pl->type, t)) return fail(Reason::access_type);
  out = *pl;
  if (use == Use::lifetime) return Flow::next;
  for (u32 c = pl->cell0; c < pl->cell0 + pl->ncells; ++c) {
    Life life = cells_[c].life;
    if (life == Life::after || (use == Use::read && life == Life::before)) return fail(Reason::outside_lifetime);
  }
  if (use == Use::write) {
    if (ob.read_only) return fail(Reason::modify_const);
    // The initializing store comes before `obj.begin`; a const object is not modified after it.
    if (shape(pl->type).is_const && pl->ncells && cells_[pl->cell0].life == Life::alive)
      return fail(Reason::modify_const);
    if (config_.mode == Mode::constant && ob.outside) return fail(Reason::modify_outside);
  } else if (config_.mode == Mode::constant && !ob.usable) {
    return fail(Reason::not_usable);
  }
  return note_access(*pl, use == Use::write);
}

// Records an access in the frame's open unsequenced groups, and reports a conflict with another
// operand of one of them ([intro.execution]/10). A callee's accesses are indeterminately
// sequenced with the caller's, so they belong to no group of the caller.
Machine::Flow Machine::note_access(const Place& p, bool write) {
  std::size_t depth = frames_.size() - 1;
  for (Group& g : groups_) {
    if (g.frame != depth) continue;
    for (const Access& a : g.accesses) {
      if (a.segment == g.segment || a.obj != p.obj || !(write || a.write)) continue;
      if (a.cell0 < p.cell0 + p.ncells && p.cell0 < a.cell0 + a.ncells) return fail(Reason::unsequenced);
    }
    g.accesses.push_back({p.obj, p.cell0, p.ncells, g.segment, write});
  }
  return Flow::next;
}

// Each evaluation of a string literal may give a distinct object ([lex.string]/9), so each gets
// its own, and comparing two of them is unspecified.
std::optional<u32> Machine::literal_object(ConstId s) {
  if (!in_->values.is<ast::VString>(s)) return std::nullopt;
  auto str = in_->values.get<ast::VString>(s);
  const Shape a = shape(str.type);
  if (a.kind != Kind::array) return std::nullopt;
  const Shape e = shape(a.inner);
  if (e.kind != Kind::integer) return std::nullopt;
  auto units = in_->seqs.elements(str.units);
  if (units.size() != a.bound) return std::nullopt;
  u32 o = make_object(str.type, Origin::literal);
  objects_[o].read_only = true;
  objects_[o].outside = true;
  objects_[o].literal = s;
  for (u32 i = 0; i < units.size(); ++i) {
    Cell& c = cells_[objects_[o].cell0 + i];
    c = {Life::alive, Fill::value, Value::integer(norm(units[i], e.width, e.is_signed))};
  }
  return o;
}

// A constant as a value of type `type`: an integer, a null pointer, or an address: of a function,
// or of an object (a string literal object or a global variable), with its byte offset and
// subobject path when stored. A global's object may first need the host.
Machine::Flow Machine::constant(ConstId c, QualType type, Value& out) {
  const ast::Values& vs = in_->values;
  const Shape& s = shape(type);
  if (vs.is<ast::VInt>(c)) {
    if (s.kind != Kind::integer) return fail(Reason::ill_formed);
    out = Value::integer(norm(vs.get<ast::VInt>(c).bits.lo, s.width, s.is_signed));
    return Flow::next;
  }
  if (vs.is<ast::VFloat>(c)) {
    if (s.kind != Kind::floating) return fail(Reason::ill_formed);
    out = Value::floating(vs.get<ast::VFloat>(c).bits.lo);
    return Flow::next;
  }
  if (vs.is<ast::VNullptr>(c)) {
    out = Value::null();
    return Flow::next;
  }
  if (!vs.is<ast::VLvalue>(c) || s.kind != Kind::pointer || s.is_nullptr) return fail(Reason::unsupported_constant);
  auto l = vs.get<ast::VLvalue>(c);
  auto indices = in_->seqs.elements(l.path);
  u32 o = 0;
  if (vs.is<ast::VString>(l.base)) {
    auto lit = literal_object(l.base);
    if (!lit) return fail(Reason::unsupported_constant);
    o = *lit;
  } else if (vs.is<ast::VEntity>(l.base)) {
    EntityId e = vs.get<ast::VEntity>(l.base).entity;
    if (shape(s.inner).kind == Kind::function) {
      if (l.offset != 0) return fail(Reason::unsupported_constant);
      out = Value::function(e);
      return Flow::next;
    }
    if (Flow fl = global_object(e, o); fl != Flow::next) return fl;
  } else {
    return fail(Reason::unsupported_constant);
  }
  if (!indices.empty() || ast::is_void(*in_, s.inner))
    return address_from_path(o, s.inner, indices, l.offset, out);
  return address(o, s.inner, l.offset, out);
}

// A path ending at an array's bound represents one past that array ([expr.add]). An offset
// one object size beyond a designated object gives the singleton's one-past position.
Machine::Flow Machine::address_from_path(u32 obj, QualType pointee, std::span<const u32> indices, i64 offset,
                                         Value& out) {
  Value pointer{VKind::object, false, obj, 0};
  for (u32 index : indices) pointer.bits = child(u32(pointer.bits), {ir::Step::elem, index});
  bool past = false;
  auto place = resolve(pointer, {}, past);
  if (!place || offset < 0 || (!ast::is_void(*in_, pointee) && !similar(place->type, pointee)))
    return fail(Reason::unsupported_constant);
  i64 start = offset_of(pointer);
  if (offset != start) {
    if (past || offset < start || u64(offset - start) != shape(place->type).size)
      return fail(Reason::unsupported_constant);
    pointer.past = true;
  }
  out = pointer;
  return Flow::next;
}

// A pointer to `pointee` at `offset` bytes into object `obj`: down through array elements until
// the type is `pointee`, or one past the end of the last such element or of the whole object.
Machine::Flow Machine::address(u32 obj, QualType pointee, i64 offset, Value& out) {
  QualType cur = objects_[obj].type;
  u32 path = 0;
  if (offset < 0) return fail(Reason::unsupported_constant);
  u64 off = u64(offset);
  for (;;) {
    const Shape cs = shape(cur);
    if (similar(cur, pointee)) {
      if (off == 0) {
        out = Value{VKind::object, false, obj, path};
        return Flow::next;
      }
      if (off == cs.size && path == 0) {
        out = Value{VKind::object, true, obj, path};
        return Flow::next;
      }
    }
    if (cs.kind != Kind::array) return fail(Reason::unsupported_constant);
    u64 es = shape(cs.inner).size;
    if (es == 0 || off / es > cs.bound) return fail(Reason::unsupported_constant);
    u64 index = off / es;
    off -= index * es;
    path = child(path, {ir::Step::elem, u32(index)});
    cur = cs.inner;
    if (index == cs.bound) {
      if (off != 0 || !similar(cur, pointee)) return fail(Reason::unsupported_constant);
      out = Value{VKind::object, false, obj, path};
      return Flow::next;
    }
  }
}

// The object of a namespace-scope variable, made on first use from what the host says of it. Its
// scalar values are decoded when first read: a value may be the address of another global.
Machine::Flow Machine::global_object(EntityId e, u32& out) {
  if (auto it = globals_.find(std::to_underlying(e)); it != globals_.end()) {
    out = it->second;
    return Flow::next;
  }
  auto info = global_info_.find(std::to_underlying(e));
  if (info == global_info_.end()) {
    wanted_ = e;
    pending_ = Event::need_global;
    return Flow::suspend;
  }
  if (!info->second) return fail(Reason::ill_formed);
  const GlobalVar g = *info->second;
  const Shape s = shape(g.type);
  constexpr u64 max_cells = std::numeric_limits<u32>::max();
  if (s.leaves == 0 || s.leaves > max_cells || cells_.size() > max_cells - s.leaves ||
      (g.value && s.kind != Kind::array && s.leaves != 1))
    return fail(Reason::unsupported_constant);
  u32 o = make_object(g.type, Origin::global);
  Object& ob = objects_[o];
  ob.entity = e;
  ob.outside = true;
  ob.usable = g.usable;
  if (g.value && s.kind == Kind::array) {
    if (!fill_array_constant(g.type, ob.cell0, *g.value)) return fail(Reason::unsupported_constant);
  } else if (g.value) {
    cells_[ob.cell0] = {Life::alive, Fill::constant, {VKind::none, false, 0, std::to_underlying(*g.value)}};
  } else {
    for (u32 c = ob.cell0; c < ob.cell0 + ob.ncells; ++c) cells_[c].life = Life::alive;
    if (config_.mode == Mode::runtime || !g.usable)
      fill_zero(g.type, ob.cell0);  // static storage is zero-initialized ([basic.start.static])
  }
  globals_.emplace(std::to_underlying(e), o);
  out = o;
  return Flow::next;
}

void Machine::give_global(EntityId e, std::optional<GlobalVar> g) {
  global_info_[std::to_underlying(e)] = std::move(g);
}

// ------------------------------------------------------------------ integers

u64 Machine::norm(u64 x, u32 width, bool is_signed) {
  if (width >= 64) return x;
  u64 mask = (u64{1} << width) - 1;
  x &= mask;
  if (is_signed && (x >> (width - 1)) & 1) x |= ~mask;
  return x;
}

Machine::Flow Machine::int_shape(Frame&, QualType t, const Shape*& s) {
  s = &shape(t);
  if (s->kind != Kind::integer) return fail(Reason::ill_formed);
  return Flow::next;
}

// add, sub, mul, neg, and, or, xor: computed modulo 2^64, then checked against the result type
// under `nsw`/`nuw` ([expr.pre]/4: a signed result outside its type's range is undefined).
Machine::Flow Machine::arith(Frame& f, NodeId n) {
  const ir::Node& nd = f.fn->node(n);
  const Shape* s;
  if (Flow fl = int_shape(f, result_type(f, n), s); fl != Flow::next) return fl;
  Value a = operand(f, n, 0), b = nd.op == Op::neg ? Value::integer(0) : operand(f, n, 1);
  if (nd.op == Op::neg) std::swap(a, b);  // 0 - x
  if (a.kind != VKind::integer || b.kind != VKind::integer) return fail(Reason::ill_formed);
  u64 x = a.bits, y = b.bits, r = 0;
  i64 sx = i64(x), sy = i64(y);
  bool sov = false, uov = false;  // signed and unsigned overflow of the exact result
  constexpr i64 max = std::numeric_limits<i64>::max(), min = std::numeric_limits<i64>::min();
  switch (nd.op) {
  case Op::add:
    r = x + y;
    sov = (sy > 0 && sx > max - sy) || (sy < 0 && sx < min - sy) || !fits(i64(r), s->width);
    uov = r < x || !fits_unsigned(r, s->width);
    break;
  case Op::sub:
  case Op::neg:
    r = x - y;
    sov = (sy < 0 && sx > max + sy) || (sy > 0 && sx < min + sy) || !fits(i64(r), s->width);
    uov = y > x;
    break;
  case Op::mul:
    r = x * y;
    sov = (sx != 0 && ((sx == -1 && sy == min) || i64(r) / sx != sy)) || !fits(i64(r), s->width);
    uov = (x != 0 && r / x != y) || !fits_unsigned(r, s->width);
    break;
  case Op::and_: r = x & y; break;
  case Op::or_: r = x | y; break;
  case Op::xor_: r = x ^ y; break;
  default: return unsupported(n);
  }
  if ((nd.flags & ir::bit(ir::Flag::nsw)) && sov) return fail(Reason::signed_overflow);
  if ((nd.flags & ir::bit(ir::Flag::nuw)) && uov) return fail(Reason::unsigned_overflow);
  return set(f, n, Value::integer(norm(r, s->width, s->is_signed)));
}

// [expr.mul]/4: division by zero is undefined, and so is a quotient that is not representable
// (then `a % b` is undefined too).
Machine::Flow Machine::divide(Frame& f, NodeId n) {
  const ir::Node& nd = f.fn->node(n);
  const Shape* s;
  if (Flow fl = int_shape(f, result_type(f, n), s); fl != Flow::next) return fl;
  Value a = operand(f, n, 0), b = operand(f, n, 1);
  if (a.kind != VKind::integer || b.kind != VKind::integer) return fail(Reason::ill_formed);
  if (b.bits == 0) return fail(Reason::division_by_zero);
  u64 r = 0;
  bool is_signed = nd.op == Op::sdiv || nd.op == Op::srem;
  if (is_signed) {
    i64 x = i64(a.bits), y = i64(b.bits);
    if (x == std::numeric_limits<i64>::min() && y == -1) return fail(Reason::quotient_overflow);
    if (!fits(x / y, s->width)) return fail(Reason::quotient_overflow);
    r = u64(nd.op == Op::sdiv ? x / y : x % y);
    if (nd.op == Op::sdiv && (nd.flags & ir::bit(ir::Flag::exact)) && x % y) return fail(Reason::inexact);
  } else {
    u64 x = norm(a.bits, s->width, false), y = norm(b.bits, s->width, false);
    r = nd.op == Op::udiv ? x / y : x % y;
    if (nd.op == Op::udiv && (nd.flags & ir::bit(ir::Flag::exact)) && x % y) return fail(Reason::inexact);
  }
  return set(f, n, Value::integer(norm(r, s->width, s->is_signed)));
}

// [expr.shift]: the count must be in [0, width of the promoted left operand); `<<` is defined
// modulo 2^N, `>>` rounds toward negative infinity on a signed operand.
Machine::Flow Machine::shift(Frame& f, NodeId n) {
  const ir::Node& nd = f.fn->node(n);
  const Shape* s;
  const Shape* cs;
  if (Flow fl = int_shape(f, result_type(f, n), s); fl != Flow::next) return fl;
  if (Flow fl = int_shape(f, type_of(f, f.fn->operands_of(n)[1]), cs); fl != Flow::next) return fl;
  Value a = operand(f, n, 0), b = operand(f, n, 1);
  if (a.kind != VKind::integer || b.kind != VKind::integer) return fail(Reason::ill_formed);
  if ((cs->is_signed && i64(b.bits) < 0) || b.bits >= s->width) return fail(Reason::shift_count, b.bits);
  u32 k = u32(b.bits);
  u64 r = 0;
  switch (nd.op) {
  case Op::shl:
    r = norm(a.bits << k, s->width, s->is_signed);
    if ((nd.flags & ir::bit(ir::Flag::nsw)) && (i64(r) >> k) != i64(a.bits)) return fail(Reason::signed_overflow);
    if ((nd.flags & ir::bit(ir::Flag::nuw)) && (norm(r, s->width, false) >> k) != norm(a.bits, s->width, false))
      return fail(Reason::unsigned_overflow);
    break;
  case Op::lshr:
    r = norm(a.bits, s->width, false) >> k;
    break;
  case Op::ashr:
    r = u64(i64(norm(a.bits, s->width, true)) >> k);
    break;
  default: return unsupported(n);
  }
  if ((nd.flags & ir::bit(ir::Flag::exact)) && k && (norm(a.bits, s->width, false) & ((u64{1} << k) - 1)))
    return fail(Reason::inexact);
  return set(f, n, Value::integer(norm(r, s->width, s->is_signed)));
}

// ------------------------------------------------------------------ floating point

// Values are computed in the type's own format, binary32 as `float` and binary64 as `double`, with
// the host's IEEE arithmetic in its default mode: round to nearest, ties to even. [expr.pre]/6
// would allow greater precision; ycxx computes as the target's code does, so both agree.
template <class T>
using FloatBits = std::conditional_t<sizeof(T) == 4, u32, u64>;
template <class T>
T decode(u64 bits) { return std::bit_cast<T>(static_cast<FloatBits<T>>(bits)); }
template <class T>
u64 encode(T x) { return std::bit_cast<FloatBits<T>>(x); }
// `x` truncated to an integer of `width` bits ([conv.fpint]); none when that does not fit.
template <class T>
std::optional<u64> truncate(T x, u32 width, bool is_signed) {
  if (!std::isfinite(x)) return std::nullopt;
  T t = std::trunc(x);
  // 2^(width - 1) and 2^width are exact in both formats.
  T top = std::ldexp(T(1), int(is_signed ? width - 1 : width));
  if (is_signed ? (t < -top || t >= top) : (t <= T(-1) || t >= top)) return std::nullopt;
  return is_signed ? u64(std::int64_t(t)) : u64(t);
}

Machine::Flow Machine::float_shape(QualType t, const Shape*& s) {
  s = &shape(t);
  if (s->kind != Kind::floating) return fail(Reason::ill_formed);
  return Flow::next;
}

// fadd, fsub, fmul, fdiv, fneg, fabs. In constant mode ([expr.const], [expr.pre]/4), a division by zero
// ([expr.mul]/4) and an arithmetic NaN result, which is not mathematically defined, are not
// constant; an overflow to an infinity is, as in Clang, and so are fneg and fabs of a NaN, which
// only change its sign. In runtime mode the operations are IEEE's.
Machine::Flow Machine::float_arith(Frame& f, NodeId n) {
  const ir::Node& nd = f.fn->node(n);
  const Shape* s;
  if (Flow fl = float_shape(result_type(f, n), s); fl != Flow::next) return fl;
  Value a = operand(f, n, 0), b = nd.op == Op::fneg || nd.op == Op::fabs ? a : operand(f, n, 1);
  if (a.kind != VKind::floating || b.kind != VKind::floating) return fail(Reason::ill_formed);
  auto compute = [&](auto x, auto y) -> Flow {
    using T = decltype(x);
    T r{};
    switch (nd.op) {
    case Op::fadd: r = x + y; break;
    case Op::fsub: r = x - y; break;
    case Op::fmul: r = x * y; break;
    case Op::fdiv:
      if (config_.mode == Mode::constant && y == T(0)) return fail(Reason::division_by_zero);
      r = x / y;
      break;
    case Op::fneg: r = -x; break;
    case Op::fabs: r = std::fabs(x); break;
    default: return unsupported(n);
    }
    if (config_.mode == Mode::constant && nd.op != Op::fneg && nd.op != Op::fabs && std::isnan(r))
      return fail(Reason::float_nan);
    return set(f, n, Value::floating(encode(r)));
  };
  if (s->width == 32) return compute(decode<float>(a.bits), decode<float>(b.bits));
  return compute(decode<double>(a.bits), decode<double>(b.bits));
}

// [expr.rel], [expr.eq]: IEEE's comparisons; a NaN is unordered, so only `!=` holds for it.
Machine::Flow Machine::float_compare(Frame& f, NodeId n) {
  const Shape* s;
  if (Flow fl = float_shape(type_of(f, f.fn->operands_of(n)[0]), s); fl != Flow::next) return fl;
  Value a = operand(f, n, 0), b = operand(f, n, 1);
  if (a.kind != VKind::floating || b.kind != VKind::floating) return fail(Reason::ill_formed);
  auto compare = [&](auto x, auto y) -> Flow {
    bool r;
    switch (ir::Pred(f.fn->imms_of(n)[0])) {
      using enum ir::Pred;
    case eq: r = x == y; break;
    case ne: r = x != y; break;
    case lt: r = x < y; break;
    case le: r = x <= y; break;
    case gt: r = x > y; break;
    case ge: r = x >= y; break;
    default: return fail(Reason::ill_formed);
    }
    return set(f, n, Value::integer(r));
  };
  if (s->width == 32) return compare(decode<float>(a.bits), decode<float>(b.bits));
  return compare(decode<double>(a.bits), decode<double>(b.bits));
}

// fconv ([conv.double], [conv.fpprom]: rounded to nearest; beyond the range, an infinity, as in
// Clang), f2i ([conv.fpint]: truncated; a value the integer type cannot hold is undefined), i2f
// ([conv.fpint]: rounded to nearest).
Machine::Flow Machine::float_convert(Frame& f, NodeId n) {
  const ir::Node& nd = f.fn->node(n);
  const Shape& to = shape(result_type(f, n));
  const Shape& from = shape(type_of(f, f.fn->operands_of(n)[0]));
  Value a = operand(f, n, 0);
  switch (nd.op) {
  case Op::fconv: {
    if (to.kind != Kind::floating || from.kind != Kind::floating || a.kind != VKind::floating) return fail(Reason::ill_formed);
    if (from.width == to.width) return set(f, n, a);
    if (to.width == 32) return set(f, n, Value::floating(encode(static_cast<float>(decode<double>(a.bits)))));
    return set(f, n, Value::floating(encode(static_cast<double>(decode<float>(a.bits)))));
  }
  case Op::f2i: {
    if (to.kind != Kind::integer || from.kind != Kind::floating || a.kind != VKind::floating) return fail(Reason::ill_formed);
    auto r = from.width == 32 ? truncate(decode<float>(a.bits), to.width, to.is_signed)
                              : truncate(decode<double>(a.bits), to.width, to.is_signed);
    if (!r) return fail(Reason::float_to_integer);
    return set(f, n, Value::integer(norm(*r, to.width, to.is_signed)));
  }
  case Op::i2f: {
    if (to.kind != Kind::floating || from.kind != Kind::integer || a.kind != VKind::integer) return fail(Reason::ill_formed);
    bool s = from.is_signed;
    if (to.width == 32) return set(f, n, Value::floating(encode(s ? static_cast<float>(i64(a.bits)) : static_cast<float>(a.bits))));
    return set(f, n, Value::floating(encode(s ? static_cast<double>(i64(a.bits)) : static_cast<double>(a.bits))));
  }
  default: return unsupported(n);
  }
}

// ------------------------------------------------------------------ pointers

// The array position of `p` seen as a pointer to `elem`: the path of the array it points into
// (or of the object itself, an array of one), its index and the array's bound ([expr.add]/1).
Machine::Flow Machine::pointer_index(const Value& p, QualType elem, u32& base_path, i64& index, u64& bound,
                                     bool& single) {
  if (p.kind != VKind::object) return fail(p.kind == VKind::null ? Reason::pointer_bounds : Reason::ill_formed);
  u32 path = u32(p.bits);
  if (path != 0 && paths_[path].step.step == ir::Step::elem && !p.past) {
    u32 parent = paths_[path].parent;
    bool past = false;
    Value arr{VKind::object, false, p.ref, parent};
    auto pl = resolve(arr, {}, past);
    if (!pl || past) return fail(Reason::ill_formed);
    const Shape& s = shape(pl->type);
    if (!similar(s.inner, elem)) return fail(Reason::pointer_type);
    base_path = parent;
    index = i64(paths_[path].step.index);
    bound = s.bound;
    single = false;
    return Flow::next;
  }
  bool past = false;
  Value whole = p;
  whole.past = false;
  auto pl = resolve(whole, {}, past);
  if (!pl || past) return fail(Reason::ill_formed);
  if (!similar(pl->type, elem)) return fail(Reason::pointer_type);
  base_path = path;
  index = p.past ? 1 : 0;
  bound = 1;
  single = true;
  return Flow::next;
}

// The byte offset of an object pointer from the start of its complete object: its address,
// relative to the object.
i64 Machine::offset_of(const Value& p) {
  QualType cur = objects_[p.ref].type;
  i64 off = 0;
  for (PathStep st : steps_of(u32(p.bits))) {
    const Shape& s = shape(cur);
    off += i64(st.index * shape(s.inner).size);
    cur = s.inner;
  }
  if (p.past) off += i64(shape(cur).size);
  return off;
}

// Whether two pointers into string literal objects could point to the same offset of one object
// ([intro.object]: potentially non-unique objects may overlap): the code units around the two
// positions agree wherever both literals have one ([expr.const]).
bool Machine::may_overlap(const Value& a, const Value& b) {
  const Object& oa = objects_[a.ref];
  const Object& ob = objects_[b.ref];
  u64 es = shape(shape(oa.type).inner).size;
  if (es == 0 || shape(shape(ob.type).inner).size != es) return false;
  i64 ia = offset_of(a) / i64(es), ib = offset_of(b) / i64(es);
  i64 na = oa.ncells, nb = ob.ncells;
  for (i64 d = -std::min(ia, ib); ia + d < na && ib + d < nb; ++d)
    if (cells_[oa.cell0 + u32(ia + d)].v.bits != cells_[ob.cell0 + u32(ib + d)].v.bits) return false;
  return true;
}

// [expr.eq], [expr.rel]: pointers into the same complete object compare by address ([expr.rel]/4:
// a later element or subobject compares greater). Pointers to different objects are unequal,
// unless one is past the end of its complete object, or both point into string literal objects
// that may overlap there: then the result is unspecified, and so is their order.
Machine::Flow Machine::compare_pointers(Frame& f, NodeId n, ir::Pred pred, const Value& a, const Value& b) {
  using enum ir::Pred;
  bool equality = pred == eq || pred == ne;
  auto result = [&](bool r) { return set(f, n, Value::integer(r)); };
  auto is_ptr = [](const Value& v) {
    return v.kind == VKind::null || v.kind == VKind::object || v.kind == VKind::function;
  };
  if (!is_ptr(a) || !is_ptr(b)) return fail(Reason::ill_formed);
  if (a.kind == VKind::object && b.kind == VKind::object && a.ref == b.ref) {
    auto c = offset_of(a) <=> offset_of(b);
    switch (pred) {
    case eq: return result(c == 0);
    case ne: return result(c != 0);
    case lt: return result(c < 0);
    case le: return result(c <= 0);
    case gt: return result(c > 0);
    case ge: return result(c >= 0);
    default: return fail(Reason::ill_formed);
    }
  }
  if (!equality) {
    if (a.kind == VKind::null && b.kind == VKind::null) return result(pred == le || pred == ge);
    return fail(Reason::unspecified_comparison);
  }
  bool same = a.kind == b.kind && (a.kind == VKind::null || a.ref == b.ref);
  if (!same && a.kind == VKind::object && b.kind == VKind::object) {
    auto at_end = [&](const Value& p) {
      i64 size = i64(shape(objects_[p.ref].type).size);
      return size != 0 && offset_of(p) == size;
    };
    if (at_end(a) || at_end(b)) return fail(Reason::unspecified_comparison);
    if (objects_[a.ref].origin == Origin::literal && objects_[b.ref].origin == Origin::literal &&
        may_overlap(a, b))
      return fail(Reason::unspecified_comparison);
  }
  return result(same == (pred == eq));
}

// ------------------------------------------------------------------ execution

Machine::Flow Machine::fail(Reason r, u64 arg) {
  failure_.reason = r;
  failure_.arg = arg;
  failure_.stack.clear();
  for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) failure_.stack.push_back({it->entity, NodeId(it->node)});
  failed_ = true;
  return Flow::stop;
}

Machine::Flow Machine::jump(Frame& f, NodeId n, u32 succ) {
  const ir::Succ& s = f.fn->succs_of(n)[succ];
  auto args = f.fn->args_of(s);
  auto params = f.fn->params_of(s.target);
  if (params.size() != args.size()) return unsupported(n);  // an edge with implicit parameters
  std::vector<Value> vals;
  vals.reserve(args.size());
  for (ir::ValueId a : args) vals.push_back(reg(f, a));
  u32 i = 0;
  for (ir::ValueId p : params) reg(f, p) = vals[i++];
  f.node = f.fn->block(s.target).node0;
  return Flow::jumped;
}

// Pushes the callee's frame; its entry block takes the state, then the arguments.
Machine::Flow Machine::enter(Frame& f, NodeId, const ir::Function* body, EntityId callee,
                             std::span<const ir::ValueId> args) {
  if (frames_.size() >= config_.depth_limit) return fail(Reason::depth_limit, config_.depth_limit);
  if (body->blocks.empty() || body->block(ir::BlockId(0)).nparams != args.size() + 1) return fail(Reason::ill_formed);
  Frame callee_frame{body, callee, std::vector<Value>(body->values.size()), body->block(ir::BlockId(0)).node0};
  u32 i = 0;
  for (ir::ValueId p : body->params_of(ir::BlockId(0)))
    if (i++ > 0) callee_frame.regs[ir::raw(p)] = reg(f, args[i - 2]);
  frames_.push_back(std::move(callee_frame));  // `f` dangles from here
  return Flow::called;
}

void Machine::start(const ir::Function& entry, EntityId name, std::span<const Value> args) {
  Frame fr{&entry, name, std::vector<Value>(entry.values.size()), 0};
  if (entry.blocks.empty() || entry.block(ir::BlockId(0)).nparams != args.size() + 1) {
    frames_.push_back(std::move(fr));
    fail(Reason::ill_formed);
    return;
  }
  u32 i = 0;
  for (ir::ValueId p : entry.params_of(ir::BlockId(0)))
    if (i++ > 0) fr.regs[ir::raw(p)] = args[i - 2];
  fr.node = entry.block(ir::BlockId(0)).node0;
  frames_.push_back(std::move(fr));
}

std::vector<Result> Machine::results() const {
  std::vector<Result> out;
  for (std::size_t i = 0; i < results_.size(); ++i) {
    const Value& v = results_[i];
    Result r;
    r.type = result_types_[i];
    switch (v.kind) {
    case VKind::none:
    case VKind::integer: r.kind = Result::Kind::integer; r.bits = v.bits; break;
    case VKind::floating: r.kind = Result::Kind::floating; r.bits = v.bits; break;
    case VKind::null: r.kind = Result::Kind::null; break;
    case VKind::function: r.kind = Result::Kind::function; r.entity = EntityId(v.ref); break;
    case VKind::object: {
      Machine& m = *const_cast<Machine*>(this);
      const Object& ob = objects_[v.ref];
      r.kind = Result::Kind::address;
      r.base = ob.origin == Origin::global ? Base::global : ob.origin == Origin::literal ? Base::literal : Base::local;
      r.entity = ob.entity;
      r.literal = ob.literal;
      r.path = steps_of(u32(v.bits));
      r.past = v.past;
      r.offset = m.offset_of(v);
      bool past = false;
      if (!r.path.empty() && m.resolve(v, {}, past)) r.past = past;
      break;
    }
    }
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace ycxx::interp
