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
  bool reference = false;  // the entity holds the address of the object this expression denotes
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
enum class BinaryOp : u8 { mul, div, rem, add, sub, lt, gt, le, ge, eq, ne, bit_and, bit_xor, bit_or };

struct Binary {
  ExprInfo info;
  BinaryOp op;
  [[=unsequenced]] Child lhs;
  [[=unsequenced]] Child rhs;
};

// The built-in three-way comparison `a <=> b` ([expr.spaceship]): `lhs` and `rhs` are prvalues of
// their common arithmetic or object pointer type. The result, a prvalue of a comparison category
// type ([cmp.categories]), is a copy of one of `results`: the static data members of that type
// that name its values, `less`, `equal` (`equivalent`), `greater` and, for
// `std::partial_ordering`, `unordered`.
struct ThreeWay {
  ExprInfo info;
  [[=unsequenced]] Child lhs;
  [[=unsequenced]] Child rhs;
  SeqId<EntityId> results;
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
enum class AssignOp : u8 { assign, mul, div, rem, add, sub, shl, shr, bit_and, bit_xor, bit_or, trivial_class_copy };

// For a compound assignment `E1 op= E2` ([expr.assign]), `computation` is the type `E1 op E2`
// is computed in: the common type of the usual arithmetic conversions, the promoted type of `E1`
// for a shift, or `E1`'s pointer type for pointer `+=` and `-=`. `rhs` is already converted to
// it (or, for a shift, promoted; for a pointer, an integer), and the result is converted back to
// `E1`'s type. A simple assignment's `rhs` is converted to `E1`'s type, and `computation` is
// `E1`'s type too.
// A `trivial_class_copy` is an assignment whose implicit operator and representation copy
// were checked by sema. The right side can designate its source object directly.
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

// A builtin that is a call of a C library function (docs/EXTENSIONS.md). Constant evaluation
// reads the bytes itself; at run time it stays the library call.
enum class LibraryBuiltin : u8 { none, memchr, memcmp };

// A call: the callee is sequenced before the arguments, which are indeterminately sequenced
// with each other ([expr.call]).
struct Call {
  ExprInfo info;
  EntityId function;  // none for a call through a pointer or object
  [[=sequenced(0)]] Child callee;
  [[=indeterminately_sequenced(1)]] ChildList args;
  LibraryBuiltin builtin = LibraryBuiltin::none;
};

// A single-object new-expression ([expr.new]): allocation is sequenced before initialization.
struct NewObject {
  ExprInfo info;
  [[=sequenced(0)]] Child allocation;
  [[=sequenced(1)]] Child init;
  bool placement = false;
};

// Single-object delete of a complete scalar or class object ([expr.delete]): the operand is
// the typed pointer prvalue. When it is non-null, the destructor (none when trivial) ends the
// object, and the usual sized deallocation function releases its storage.
struct DeleteObject {
  ExprInfo info;
  EntityId function;
  [[=sequenced(0)]] Child address;
  [[=sequenced(1)]] Child size;
  EntityId destructor = EntityId::none;
};

// One-dimensional array new ([expr.new]). The bound is converted to size_t, and the
// initializer describes each element. A validated cookie-free allocation has a call.
struct NewArray {
  ExprInfo info;
  [[=sequenced(0)]] Child bound;
  [[=sequenced(1)]] Child allocation;
  [[=sequenced(2)]] Child element_init;
};

// Array delete ([expr.delete]) preserves the typed pointer operand and the usual unsized deallocator.
struct DeleteArray {
  ExprInfo info;
  EntityId function;
  [[=sequenced]] Child operand;
};

// Built-in functions handled during evaluation (docs/EXTENSIONS.md).
enum class BuiltinOp : u8 { fabs, strlen, trap, unreachable, add_overflow, mul_overflow, ctz };
struct BuiltinCall {
  ExprInfo info;
  BuiltinOp op;
  [[=indeterminately_sequenced]] ChildList args;
};

// The written destination type and source expression of __builtin_bit_cast.
struct BitCast {
  ExprInfo info;
  QualType destination;
  [[=sequenced]] Child source;
  u32 type_at;
  bool supported;
};

// A folded __builtin_constant_p result. The operand is retained for expression analysis,
// including noexcept, but is never executed.
struct ConstantProbe {
  ExprInfo info;
  [[=sequenced]] Child operand;
  bool proved;
};

// Variadic operations ([cstdarg.syn]). Operands are the normalized cursor addresses.
enum class VaOp : u8 { start, arg, copy, end };
struct VariadicBuiltin {
  ExprInfo info;
  VaOp op;
  [[=indeterminately_sequenced]] ChildList args;
};

// The standard conversions ([conv]) and the others an implicit conversion sequence is made of.
// `integral` covers integral promotions ([conv.prom]) and conversions ([conv.integral]), told
// apart by the types, and `floating` floating-point promotions ([conv.fpprom]) and conversions
// ([conv.double]); `floating_integral` goes either way between the two ([conv.fpint]); `to_void`
// is a discarded-value expression's conversion ([expr.context]).
enum class ConversionKind : u8 {
  lvalue_to_rvalue, integral, floating_integral, user_defined, derived_to_base,
  array_to_pointer, function_to_pointer, qualification, boolean, null_pointer, to_void,
  pointer,  // [conv.ptr]: an object pointer to `cv void*`
  floating,
  array_copy,  // [dcl.struct.bind]: initialize the hidden array, element by element
  base_to_derived,  // [expr.static.cast]: undo a direct base adjustment
  pointer_to_integral, integral_to_pointer,  // [expr.reinterpret.cast]
};

// A conversion node of width zero beyond its operand (§3.3). `function` names a user-defined
// conversion function or the direct base class in a class adjustment.
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
// scope ([class.temporary]/6). `extended` 2 marks a call's argument object for a parameter of class
// type ([expr.call]/7), passed by value; it too ends with the full-expression.
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

// An explicit destructor call ([class.dtor]): the object ends its lifetime without
// releasing its storage. A trivial destructor has no function to call.
struct ExplicitDestroy {
  ExprInfo info;
  EntityId function;
  [[=sequenced]] Child object;  // the object's address
};

// The ABI slot relative to the table's address point, and the statically selected member
// that supplies the default arguments.
struct VirtualMemberSelection {
  u32 slot;
  EntityId function = EntityId::none;  // none for peer-only virtual call profiles
  constexpr bool operator==(const VirtualMemberSelection&) const = default;
};

// An unqualified ordinary virtual call ([class.virtual]).
struct VirtualMemberCall {
  ExprInfo info;
  QualType callee;  // pointer to the ABI function type, which includes the receiver parameter
  SeqId<VirtualMemberSelection> selection;  // one record keeps the payload within the inline budget
  [[=sequenced(0)]] Child object;
  [[=indeterminately_sequenced(1)]] ChildList args;
};

// An immediate virtual member call ([expr.const.core], [class.virtual]). `function` is the
// statically selected member, which also supplies any default arguments. Only an unqualified
// call needs the receiver's dynamic type.
struct ImmediateVirtualCall {
  ExprInfo info;
  EntityId function;
  [[=sequenced(0)]] Child object;
  [[=indeterminately_sequenced(1)]] ChildList args;
  u8 qualified;
};

// `dynamic_cast<cv void*>(p)` ([expr.dynamic.cast]): the address of the most derived object.
struct DynamicCastVoid {
  ExprInfo info;
  [[=sequenced]] Child operand;
};

// A typed runtime pointer cast ([expr.dynamic.cast]), with opaque Itanium RTTI imports.
struct DynamicCastPointer {
  ExprInfo info;
  EntityId source_rtti;
  EntityId target_rtti;
  EntityId runtime;
  [[=sequenced]] Child operand;
};

// Assignment notation calls a member function with the argument sequenced before its object
// ([expr.assign], [over.match.oper]). `object` is the implicit pointer argument.
struct AssignmentCall {
  ExprInfo info;
  EntityId function;
  [[=sequenced(0)]] Child rhs;
  [[=sequenced(1)]] Child object;
};

// Aggregate initialization after analysis ([dcl.init.aggr], [dcl.init.general]). The sequence
// contains one converted initializer per aggregate element, with omissions filled in and any
// brace elision and designators resolved. A sub-aggregate's initializer is an `InitList` or a
// prvalue of its type. Element initializations are sequenced in order.
struct InitList {
  ExprInfo info;
  [[=sequenced]] ChildList elems;
};

// A lambda-expression ([expr.prim.lambda]): a prvalue of its closure type, whose captures are
// initialized in field order by `inits`, `MemberInit`s ([expr.prim.lambda.capture]/15).
struct Lambda {
  ExprInfo info;
  EntityId closure;
  [[=sequenced]] ChildList inits;
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

// Default-initialization of a complete automatic object with an Itanium vtable.
struct PeerRootInit {
  ExprInfo info;
  EntityId vtable;
};

// An omitted field initialized from its default member initializer ([class.mem.general]).
// Its child was analysed at the declaration; `this` is the enclosing InitList's destination.
struct DefaultMemberInit {
  ExprInfo info;
  [[=sequenced]] Child init;
};

// A type- or value-dependent construct of a template's definition ([temp.dep]), analysed no
// further: its operands as written. Its type is `Builtin::dependent`, or the type [temp.dep.expr]
// fixes when the construct is only value-dependent. A template's bodies are instantiated by
// analysing their tokens again with the template arguments bound (ast.md §6.4 as amended by #88),
// so this kind needs no payload beyond its operands.
struct Dependent {
  ExprInfo info;
  [[=indeterminately_sequenced]] ChildList operands;
};

// The name of an unqualified function call that lookup did not find at a template's definition
// ([temp.dep.candidate]): argument-dependent lookup at the point of instantiation may.
struct DepName {
  ExprInfo info;
  NameId name;
};

// ---------------------------------------------------------------- structure

struct ExprStmt {
  Child expr;
};

// The destructors a scope exit runs are not listed on the exiting statement: each object names
// its own (`LocalVar::destructor`, `Materialize::destructor`, `DestroyMember`), and `function_ir`
// derives every exit edge's calls from the scopes it leaves (ast.md §3.7.3, #135).
struct Return {
  Child value;
};

struct Compound {
  ChildList stmts;
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

// A statement-position asm-declaration ([dcl.asm]) with an ordinary string.
struct Asm {
  ConstId text;
};

// GNU extended asm with an empty template, no operands and a single memory clobber.
struct AsmMemoryBarrier {};

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

// [stmt.break], [stmt.cont].
struct Break {};
struct Continue {};

// A block-scope simple-declaration ([stmt.dcl]): its `LocalVar`s, in order.
struct DeclStmt {
  ChildList decls;
};

// A condition that declares a variable ([stmt.pre]/4): the `LocalVar`, then the variable's value
// converted to bool, or for a `switch` promoted ([stmt.switch]/5). It stands where the condition's
// expression would.
struct CondDecl {
  Child var;
  Child value;
};

// [stmt.switch]: `init` is a statement or none; `condition` is the promoted integral or
// enumeration value (or a `CondDecl`). Its `Case` and `Default` labels are in `body`, outside any
// nested switch.
struct Switch {
  Child init;
  Child condition;
  Child body;
};

// [stmt.label]: a case label, its value converted to the condition's type (none in a template's
// definition while it is value-dependent), and the statement it labels, which is none at the end
// of a compound statement.
struct Case {
  ConstId value;
  Child stmt;
};
struct Default {
  Child stmt;
};
// An identifier label; labels have function scope ([stmt.label]/2).
struct Label {
  NameId name;
  Child stmt;
};
// [stmt.goto]
struct Goto {
  NameId label;
};

// A call's argument taken from its parameter's default argument ([dcl.fct.default]): the tree
// analysed at the declaration, converted to the parameter's type. Its nodes are positioned at the
// call, which is in another item.
struct DefaultArg {
  ExprInfo info;
  [[=sequenced]] Child arg;
  // Written token range in the declaration item, present for a dependent default.
  ItemId source{};
  u32 source_begin = 0;
  u32 source_end = 0;
  SeqId<u32> dependent_calls{};
};

// A source-line expression in a default argument. An early constant evaluation uses its
// lexical line. Evaluation at an actual call uses the enclosing source-line context.
struct SourceLine {
  ExprInfo info;
  u32 lexical_line;
};

// A source-location descriptor pointer in a default argument. Its value is bound at the call.
struct SourceLocation {
  ExprInfo info;
};

// A default expression that observes the invocation site, including through nested defaults.
// The declaration records the observations; use of the argument supplies the values.
struct DefaultSourceContext {
  ExprInfo info;
  [[=sequenced]] Child arg;
  bool observes_line = false;
  u32 line = 0;
  EntityId descriptor_type = EntityId::none;
  ConstId descriptor_address = ConstId::none;
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

// A delegating constructor initializes the same complete object before its body ([class.base.init]).
struct DelegatingInit {
  Child call;
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

// The dynamic initialization of a namespace-scope variable ([basic.start.dynamic]), a statement
// of the unit's initialization function: `init`, the variable's initializer from its declaration
// (another item's tree, so its nodes take this node's position), initializes the object of
// `variable`, of type `type`. Its `destructor`, if any, is then registered to run at exit
// ([basic.start.term]) through `atexit` (`__cxa_atexit`) with `dso` (`__dso_handle`), the
// image's handle (Itanium C++ ABI §3.3.5).
struct StaticInit {
  EntityId variable;
  QualType type;
  Child init;
  EntityId destructor;
  EntityId atexit;
  EntityId dso;
};

// A block-scope variable with static storage duration ([stmt.dcl]/3): `BodySema::locals[local]`
// names its entity. `value` is the constant it is initialized with ([basic.start.static]);
// otherwise `init`, if any, initializes it the first time control passes here, once, under its
// guard, and then `destructor`, if any, is registered to run at exit ([basic.start.term]).
struct StaticVar {
  u32 local;
  Child init;
  ConstId value;
  EntityId destructor;
  bool usable_in_constant_expressions;  // control may pass the declaration ([expr.const])
};

struct Attribute {
  NameId scope;  // NameId of the attribute-namespace, or of "" for none
  NameId name;
  ChildList args;
};

// [expr.throw]: a rethrow has no operand.
struct Throw {
  ExprInfo info;
  [[=sequenced]] Child operand;
  EntityId exception_class = EntityId::none;  // a certified class exception object
};

// [except.pre], [except.handle]: the protected block and its ordered handlers.
struct TryBlock {
  Child body;
  ChildList handlers;
};

enum class HandlerKind : u8 { typed, catch_all, invalid };

struct Handler {
  HandlerKind kind;
  QualType type;  // builtin void for a catch-all or invalid declaration
  u32 local;  // no local for an abstract declaration or catch (...)
  Child body;
};

using Nodes = closed_list<Literal, DeclRef, Paren, Unary, Binary, OrderedBinary, Assign, Subscript, Call,
                          ImplicitConversion, ExprStmt, Return, Compound, If, LocalVar, Attribute, LocalRef,
                          Conditional, While, Do, For, Break, Continue, DeclStmt, Declarator, DeclGroup,
                          IfConsteval, StaticAssert, Member, InitList, ValueInit, DefaultInit, This,
                          MemberCall, Materialize, CtorCall, MemberInit, Destructor, DestroyMember, Dependent,
                          DepName, CondDecl, Switch, Case, Default, Label, Goto, BuiltinCall, Lambda, DefaultArg,
                          StaticInit, StaticVar, NewObject, DeleteObject, VariadicBuiltin, AssignmentCall,
                          NewArray, DeleteArray, DelegatingInit, VirtualMemberCall, Throw, TryBlock, Handler,
                          DefaultMemberInit, ExplicitDestroy, PeerRootInit, DynamicCastVoid, DynamicCastPointer, Asm,
                          ConstantProbe, ThreeWay, SourceLine, DefaultSourceContext, SourceLocation,
                          ImmediateVirtualCall, BitCast, AsmMemoryBarrier>;

static_assert(Nodes::size < max_node_kinds);

template <class T>
constexpr bool is_node_kind = Nodes::contains<T>;

// Every kind is checked: permitted members, trivially copyable, payload at most 32 bytes (§9.6),
// sequencing annotations where they belong (§3.7.4).
static_assert(std::ranges::all_of(Nodes::infos, check_node_kind));

}  // namespace ycxx::ast
