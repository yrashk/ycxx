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

// A name that resolved to a block-scope variable or a parameter: an index into
// `BodySema::locals`, whose first entries are the function's parameters (§4.4).
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

// For a compound assignment `E1 op= E2` ([expr.assign]), `computation` is the type `E1 op E2`
// is computed in: the common type of the usual arithmetic conversions, the promoted type of `E1`
// for a shift, or `E1`'s pointer type for pointer `+=` and `-=`. `rhs` is already converted to
// it (or, for a shift, promoted; for a pointer, an integer), and the result is converted back to
// `E1`'s type. A simple assignment's `rhs` is converted to `E1`'s type, and `computation` is
// `E1`'s type too.
struct Assign {
  ExprInfo info;
  AssignOp op;
  [[=sequenced(1)]] Child lhs;
  [[=sequenced(0)]] Child rhs;
  QualType computation;
};

// `c ? a : b` ([expr.cond]): the condition is sequenced before the operand that is evaluated,
// and only one of `then_` and `else_` is evaluated. The condition is converted to bool; the two
// operands are converted to the result type.
struct Conditional {
  ExprInfo info;
  [[=sequenced(0)]] Child condition;
  [[=sequenced(1)]] Child then_;
  [[=sequenced(2)]] Child else_;
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

// The standard conversions ([conv]) and the others an implicit conversion sequence is made of.
// `integral` covers integral promotions ([conv.prom]) and conversions ([conv.integral]), told
// apart by the types; `to_void` is a discarded-value expression's conversion ([expr.context]).
enum class ConversionKind : u8 {
  lvalue_to_rvalue, integral, floating_integral, user_defined, derived_to_base,
  array_to_pointer, function_to_pointer, qualification, boolean, null_pointer, to_void,
};

// An implicit conversion: a node of width zero beyond its operand (§3.3), naming its function for
// a user-defined conversion.
struct ImplicitConversion {
  ExprInfo info;
  ConversionKind conversion;
  EntityId function;
  [[=sequenced]] Child operand;
};

// Class member access ([expr.ref]): `object.member`, or with `arrow` 1 `object->member`, whose
// object is then the pointer. `field` is the member's position among its class's non-static data
// members, the access path step the IR takes. With `reference` 1 the member is a reference, and
// the result is the object it refers to ([expr.ref]/7.2).
struct Member {
  ExprInfo info;
  EntityId member;
  u32 field;
  u32 arrow;
  u32 reference;
  [[=sequenced]] Child object;
};

// Temporary materialization ([conv.rval], [class.temporary]): a temporary object initialized from
// the prvalue `operand`, as an xvalue. It is destroyed by `destructor` (none if trivial) at the end
// of the full-expression, or, with `extended` 1, when the reference it was bound to goes out of
// scope ([class.temporary]/6).
struct Materialize {
  ExprInfo info;
  EntityId destructor;
  u32 extended;
  [[=sequenced]] Child operand;
};

// A constructor call initializing a prvalue's result object ([class.ctor], [dcl.init.general]):
// `T(args)`, `T{args}`, or an initialization that resolved to `ctor`. With `zero_first` 1 the
// object is zero-initialized first (value-initialization, [dcl.init.general]/9).
struct CtorCall {
  ExprInfo info;
  EntityId ctor;
  u32 zero_first;
  [[=indeterminately_sequenced]] ChildList args;
};

// `this` ([expr.prim.this]): a prvalue pointer to the object a non-static member function was
// called on. An implicit member access ([class.mfct.non.static]) names it with width 0.
struct This {
  ExprInfo info;
};

// A call of a non-static member function ([expr.call], [class.mfct.non.static]): `object` is the
// implicit object argument as a pointer prvalue (`&a` for `a.f()`, `p` for `p->f()`, `this` for an
// implicit call), sequenced before the arguments.
struct MemberCall {
  ExprInfo info;
  EntityId function;
  [[=sequenced(0)]] Child object;
  [[=indeterminately_sequenced(1)]] ChildList args;
};

// List-initialization of an aggregate ([dcl.init.aggr]), as analysed: one element per array
// element or non-static data member, in order, with brace elision and designators resolved and the
// elements without an initializer value-initialized. Each element is already converted to its
// element type; a sub-aggregate's element is an `InitList` or a prvalue of its type. The
// initializer-clauses are sequenced in order ([dcl.init.list]).
struct InitList {
  ExprInfo info;
  [[=sequenced]] ChildList elems;
};

// Value-initialization of an object of the type ([dcl.init.general]): for a scalar, zero; for an
// aggregate without constructors, every element value-initialized.
struct ValueInit {
  ExprInfo info;
};

// Default-initialization of an object of scalar or aggregate type that performs no
// initialization ([dcl.init.general]/7.3): its value is erroneous, or indeterminate.
struct DefaultInit {
  ExprInfo info;
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

// [stmt.if]: `if consteval`, or with `negated` 1 `if !consteval`. The first substatement runs when
// the statement is evaluated in a manifestly constant-evaluated context.
struct IfConsteval {
  u32 negated;
  Child then_;
  Child else_;
};

// [dcl.pre]: `static_assert(condition, message)`, checked when it was analysed. The condition is
// converted to bool; the message is a string literal's `VString`, or none.
struct StaticAssert {
  Child condition;
  ConstId message;
};

// [stmt.while]: the condition is converted to bool.
struct While {
  Child condition;
  Child body;
};

// [stmt.do]
struct Do {
  Child body;
  Child condition;
};

// [stmt.for]: `init` is a statement; `condition` and `increment` may be absent.
struct For {
  Child init;
  Child condition;
  Child increment;
  Child body;
};

// [stmt.break], [stmt.cont]: the destructor calls on the exit edge, in order (§3.7.3).
struct Break {
  SeqId<Cleanup> cleanups;
};
struct Continue {
  SeqId<Cleanup> cleanups;
};

// A block-scope simple-declaration ([stmt.dcl]): its `LocalVar`s, in order.
struct DeclStmt {
  ChildList decls;
};

// ---------------------------------------------------------------- declarations (item trees)

// One declarator of an item ([dcl.decl]): the ordinal of its record in `ItemSema::decls`, its
// initializer, and, for a variable with static storage duration whose initializer is a constant
// expression, the constant it is initialized with ([basic.start.static]); `ConstId::none` means
// zero-initialization when there is no initializer.
struct Declarator {
  u32 decl;
  Child init;
  ConstId value;
};

// The root of a declaration's item tree: its declarators in order.
struct DeclGroup {
  ChildList declarators;
};

// A block-scope variable: an index into `BodySema::locals` (§4.4), its initializer, and the
// destructor that ends it at scope exit, or none ([stmt.dcl], [class.dtor]). For an array it
// destroys each element, the last first.
struct LocalVar {
  u32 local;
  Child init;
  EntityId destructor;
};

// A non-static data member's initialization in a constructor ([class.base.init]): from its
// mem-initializer, its default member initializer, or default-initialized. The statements of a
// constructor's body come in declaration order before the compound statement.
struct MemberInit {
  u32 field;
  QualType type;  // the member's declared type
  Child init;
};

// A destructor's body ([class.dtor]): `body`, then on every way out of it the destruction of the
// members (`DestroyMember`s, in reverse declaration order).
struct Destructor {
  Child body;
  ChildList members;
};
struct DestroyMember {
  u32 field;
  QualType type;  // the member's declared type; an array's elements are destroyed last first
  EntityId destructor;
};

struct Attribute {
  NameId scope;  // NameId of the attribute-namespace, or of "" for none
  NameId name;
  ChildList args;
};

using Nodes = closed_list<Literal, DeclRef, Paren, Unary, Binary, OrderedBinary, Assign, Subscript, Call,
                          ImplicitConversion, ExprStmt, Return, Compound, If, LocalVar, Attribute, LocalRef,
                          Conditional, While, Do, For, Break, Continue, DeclStmt, Declarator, DeclGroup,
                          IfConsteval, StaticAssert, Member, InitList, ValueInit, DefaultInit, This,
                          MemberCall, Materialize, CtorCall, MemberInit, Destructor, DestroyMember>;

static_assert(Nodes::size < max_node_kinds);

template <class T>
constexpr bool is_node_kind = Nodes::contains<T>;

// Every kind is checked: permitted members, trivially copyable, payload at most 32 bytes (§9.6),
// sequencing annotations where they belong (§3.7.4).
static_assert(std::ranges::all_of(Nodes::infos, check_node_kind));

}  // namespace ycxx::ast
