// The node kinds: one closed list, `Nodes`, of plain aggregates (docs/design/ast.md §3.3). Nodes
// hold no position; a child edge holds its offset relative to the parent. Fingerprint, equality,
// walking, dispatch and compaction are derived from these definitions by reflection, so adding a
// kind is adding a struct and listing it.
//
// This first list is the core's: enough expression and structure kinds to exercise the
// machinery, including every sequencing shape of §3.7.4. The parser (M2) and sema (M3) add the
// rest of the language's kinds here.
export module ycxx.ast.nodes;

import std;
import ycxx.ast.handles;
import ycxx.ast.schema;

export namespace ycxx::ast {

// ---------------------------------------------------------------- expressions

// A literal; its value is a constant (§5.5).
struct Literal {
  ExprInfo info;
  ConstId value;
};

// A name that resolved to an entity.
struct DeclRef {
  ExprInfo info;
  EntityId entity;
};

// A name of a block-scope variable or a parameter: an index into `BodySema::locals` (§4.4).
struct LocalRef {
  ExprInfo info;
  u32 local;
};

// Parentheses change meaning (`decltype((x))`, `(f)(x)`), so they are a kind (§3.4).
struct Paren {
  ExprInfo info;
  [[=sequenced]] Child operand;
};

enum class UnaryOp : u8 { plus, minus, not_, compl_, deref, address_of, pre_inc, pre_dec, post_inc, post_dec };

struct Unary {
  ExprInfo info;
  UnaryOp op;
  [[=sequenced]] Child operand;
};

// The binary operators whose operands are unsequenced ([intro.execution]).
enum class BinaryOp : u8 { mul, div, rem, add, sub, lt, gt, le, ge, spaceship, eq, ne, bit_and, bit_xor, bit_or };

struct Binary {
  ExprInfo info;
  BinaryOp op;
  [[=unsequenced]] Child lhs;
  [[=unsequenced]] Child rhs;
};

// The binary operators whose left operand is sequenced before the right: shifts ([expr.shift]),
// `&&` and `||` ([expr.log.and], [expr.log.or]), the comma ([expr.comma]) and pointer-to-member
// access ([expr.mptr.oper]).
enum class OrderedOp : u8 { shl, shr, logical_and, logical_or, comma, dot_star, arrow_star };

struct OrderedBinary {
  ExprInfo info;
  OrderedOp op;
  [[=sequenced(0)]] Child lhs;
  [[=sequenced(1)]] Child rhs;
};

// Simple and compound assignment: the right operand is sequenced before the left
// ([expr.assign]).
enum class AssignOp : u8 { assign, mul, div, rem, add, sub, shl, shr, bit_and, bit_xor, bit_or };

struct Assign {
  ExprInfo info;
  AssignOp op;
  [[=sequenced(1)]] Child lhs;
  [[=sequenced(0)]] Child rhs;
};

// `a[b]`: `a` is sequenced before `b` ([expr.sub]).
struct Subscript {
  ExprInfo info;
  [[=sequenced(0)]] Child base;
  [[=sequenced(1)]] Child index;
};

// A call: the callee is sequenced before the arguments, which are indeterminately sequenced
// with each other ([expr.call]).
struct Call {
  ExprInfo info;
  EntityId function;  // none for a call through a pointer or object
  [[=sequenced(0)]] Child callee;
  [[=indeterminately_sequenced(1)]] ChildList args;
};

// `c ? a : b`: the condition is sequenced before the arm that is evaluated ([expr.cond]).
struct Conditional {
  ExprInfo info;
  [[=sequenced(0)]] Child condition;
  [[=sequenced(1)]] Child then_;
  [[=sequenced(1)]] Child else_;
};

enum class ConversionKind : u8 {
  lvalue_to_rvalue, integral, floating_integral, user_defined, derived_to_base,
  boolean,           // [conv.bool]
  array_to_pointer,  // [conv.array]
  qualification,     // [conv.qual]
  null_pointer,      // [conv.ptr]
};

// An implicit conversion: a node of width zero beyond its operand (§3.3), naming its function for
// a user-defined conversion.
struct ImplicitConversion {
  ExprInfo info;
  ConversionKind conversion;
  EntityId function;
  [[=sequenced]] Child operand;
};

// ---------------------------------------------------------------- structure

// A destructor call on a scope-exit edge (§3.7.3): the body local and the destructor.
struct Cleanup {
  u32 local;
  EntityId destructor;
  constexpr bool operator==(const Cleanup&) const = default;
};

struct ExprStmt {
  Child expr;
};

struct Return {
  Child value;
  SeqId<Cleanup> cleanups;  // in order, computed by analyze_body
};

struct Compound {
  ChildList stmts;
  SeqId<Cleanup> cleanups;  // on normal exit
};

struct If {
  Child init;
  Child condition;
  Child then_;
  Child else_;
};

// Iteration statements ([stmt.iter]); an absent part of a `for` is `NodeId::none`.
struct While {
  Child condition;
  Child body;
};

struct Do {
  Child body;
  Child condition;
};

struct For {
  Child init;
  Child condition;
  Child increment;
  Child body;
};

// Jump statements ([stmt.jump]) with their scope-exit cleanups (§3.7.3).
struct Break {
  SeqId<Cleanup> cleanups;
};

struct Continue {
  SeqId<Cleanup> cleanups;
};

// The null statement `;` ([stmt.expr]).
struct Null {};

// A block-scope variable: an index into `BodySema::locals` (§4.4) and its initializer.
struct LocalVar {
  u32 local;
  Child init;
};

struct Attribute {
  NameId scope;  // NameId of the attribute-namespace, or of "" for none
  NameId name;
  ChildList args;
};

using Nodes = closed_list<Literal, DeclRef, Paren, Unary, Binary, OrderedBinary, Assign, Subscript, Call,
                          ImplicitConversion, ExprStmt, Return, Compound, If, LocalVar, Attribute, LocalRef,
                          Conditional, While, Do, For, Break, Continue, Null>;

static_assert(Nodes::size < max_node_kinds);

template <class T>
constexpr bool is_node_kind = Nodes::contains<T>;

// Every kind is checked: permitted members, trivially copyable, payload at most 32 bytes (§9.6),
// sequencing annotations where they belong (§3.7.4).
static_assert(std::ranges::all_of(Nodes::infos, check_node_kind));

}  // namespace ycxx::ast
