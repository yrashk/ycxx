# AST, entity and type representation

Design for [yrashk/ycxx#2](https://github.com/yrashk/ycxx/issues/2). Status: **proposed**. When it
is accepted, §1 is summarized into DECISIONS §4.

This document decides how ycxx represents what it parses and analyses: syntax and semantic trees,
declarations and entities, types, templates, C++26 constructs, source locations, and the memory
that holds them. It builds on the accepted [query-engine design](query-engine.md), cited as §A…
and §B…, and takes its Part D as binding: items as the unit of parsing, `(item, offset)`
positions, declaration and entity ids as the only references between items, results as values,
and nothing context-specific in a shared result.

| Issue question | Where |
| --- | --- |
| Syntax tree versus semantic entities | §3 |
| Stable identities that survive edits | §4 |
| Type representation and canonicalization | §5 |
| Templates: pattern versus instantiation, dependent constructs | §6 |
| Source locations through macros and includes | §8 |
| Reflection, expansion statements, contracts, pack indexing | §7 |
| Memory, arenas and interning | §9 |

## Provenance and who decided what

Sources, per DECISIONS §2: the C++26 working draft (cited by stable clause name), WG21 papers
(P2996, P2900, P1306, P2662, P3394), the Itanium C++ ABI, and Roslyn's red–green trees as publicly
described. No compiler's source code or design documents were consulted.

Following DECISIONS §6.5, the decision-bearing questions went to two Fable subagents, and the
Opus session that owns the issue wrote the spike, this document and the bookkeeping.

- **Fable, first brief:** the handle vocabulary and its invariants (§2), syntax versus semantics
  (§3), identities (§4) and memory (§9).
- **Fable, second brief, built on the first:** types (§5), templates (§6), C++26 constructs (§7)
  and locations (§8).
- **The Opus session** wrote the spike (§10), reviewed both sets of decisions and sent pushback
  where noted, and wrote this text.
- **Review.** One independent Fable check of the assembled document found no P0, 9 P1 and 15
  P2, each with a failing example and a smallest fix; all were applied in one commit. A check of
  that diff alone closed 23 of them and found that one proposed fix (default arguments carried by
  `instantiate_fn`) would make a call depend on the END instantiation; the session applied the
  narrow remedy (§6.4) without a further round.

## 1. Decisions in brief

1. **Handles, not pointers.** Every reference is a 32-bit handle into a typed, process-wide
   interner, or an 8-byte structural id (`DeclId`, `Loc`). Values hold nothing context-specific,
   and fingerprints, never handles, cross process boundaries. (§2)
2. **The splitter yields skeletons; the parser analyses as it parses.** Items are cut and indexed
   without lookup. Parsing runs inside `item_sema` and `analyze_body`, because C++ cannot be
   parsed without knowing which names are types and templates. (§3.1, §3.2)
3. **One tree family.** Structure and expression nodes, with implicit operations explicit and
   dependent constructs as kinds of the same family. Nodes hold no position: a child edge carries
   its offset relative to the parent, so equal subtrees are one node, and a red view on the stack
   computes positions. (§3.3–§3.5)
4. **Identities.** `DeclId = (ItemId, ordinal)` with written, synthesized and injected classes;
   none inside bodies. Entity ids are structural, with local entities numbered as the Itanium ABI
   does. Class members are analysed at the class item's prefix, and complete-class contexts at the
   prefix after it, which needs one additive engine operation. (§4)
5. **Types.** One closed list of canonical and sugar kinds; `QualType` is a node with cv in its
   low bits; the canonical form is a derived array filled at interning, so canonicalization is
   O(1) and independent of interning order. Expressions carry the sugared type. (§5.1–§5.4)
6. **Dependent values are constants with free parameters.** In canonical positions (template
   arguments, bounds, `decltype` operands), dependent expressions are `VDep*` constants, so no tree
   enters a type or a key, and substitution maps constants to constants. (§5.5, §5.6)
7. **Templates.** A template is analysed by the ordinary cells, with dependent kinds and stored
   definition-context lookup sets. Substitution is a function whose lookups are the cells.
   Instantiation builds a fresh tree that keeps the pattern's offsets. (§6)
8. **C++26.** A reflection is a constant over entity, type, value and positional handles, compared
   by alias identity or canonical type. Expansion statements store either the pattern or the
   instances. Contracts are declaration data. Pack indexing resolves to the selected element.
   (§7)
9. **Locations.** Tokens carry none; the region keeps a parallel table of byte offsets and macro
   expansions. `Loc` offsets count tokens. Rendering goes through a per-TU item index and
   per-file line tables. Positions that become constant values go through a recorded question.
   (§8)
10. **Memory.** Trees are built in the frame's arena and interned when the frame commits; types,
    names, entities and constants are interned eagerly because they are keys. Per-kind dense pools,
    64 bytes per distinct node, body trees evictable. (§9)
11. **The IR boundary.** `function_ir` sees no dependent construct, only canonical types and
    `ConstId`s, and keeps `Loc`s in a side table the optimizer never sees. (§11)

## 2. Handles and invariants

Every value below is a structural aggregate in the sense of §A10.1. Fingerprint, equality,
serialization and handle walking are derived by reflection, as the spike does.

| Handle | Denotes | Lives in | In keys |
| --- | --- | --- | --- |
| `NameId` (u32) | an identifier, operator-function-id or literal-operator-id | `names` interner | yes |
| `TokSeqId` (u32) | a preprocessed token sequence | `tokens` | yes |
| `BodyId` (u32) | a function body's tokens (§B1.2) | `tokens` | yes |
| `ItemId` (u32) | an item: file identity, scope path, declaration-part tokens, ordinal, occurrence salt and skeleton (§3.1); its fingerprint is §B1.2's item id | `items` | yes |
| `DeclId` (8 bytes) | `{ItemId, DeclOrd}`: one declared name of an item (§4.1) | structural, not interned | yes |
| `EntityId` (u32) | a structural, program-wide entity (§B1.5, §4.3) | `entities` | yes |
| `NodeId` (u32) | a tree node: kind in 8 bits, index in 24 | `ast`, one pool per kind | allowed, unused |
| `QualType` (u32) | a type node in 29 bits and its top-level cv in 3 | `types` | yes |
| `ConstId` (u32) | a constant value (§5) | `values` | yes |
| `SeqId<T>` (u32) | an interned ordered list (`DeclListId`, `TypeListId`, `ScopePathId`, …) | `seq<T>` per element type | yes |
| `Loc` (8 bytes) | `{ItemId, token offset}` (§8) | structural | never |
| `Prefix`, `PinsId`, `ChainId`, `CellId` | the engine's (§A1.9) | engine | keys only |

`Loc` appears only in diagnostics, demand lists and debug side tables. A `NodeId` appears only in
the value of the cell that built its tree or of a cell that fetched that value. The engine's
handles appear in no value. Everything else may appear anywhere, IR included. Handles are strong
enums, and the set of handle types is one closed list, `handles<…>`, which is what compaction
walks (§9).

Invariants every part of the front end keeps:

1. **Nothing context-specific in a value.** No node, record or value holds a `Prefix`, a
   `PinsId`, a `CellId`, a TU identity, an absolute file offset, a raw pointer or a handle outside
   `handles<…>` (Part D, §A1.9).
2. **Nodes hold no position.** A child edge is `{child, offset relative to the parent}`, and a
   root is at offset 0 of its part, so equal subtrees are one node (spike, §10).
3. **Fingerprints are Merkle through handles** and so independent of interning order (§A1.3).
4. **Interned content is immutable and wholly identity.** A tree node has no mutable field and no
   field excluded from its fingerprint; nodes are shared across frames and TUs. Annotations may
   add behaviour (`[[=sugar]]`, §5), never remove a field from identity.
5. **Handles are local to the session.** They are never persisted or shown to clients, and
   compaction renumbers them. Fingerprints are what crosses process boundaries (§A7.1).
6. **A node refers outside its own tree only by** `DeclId`, `EntityId`, `ItemId`, `BodyId`,
   `QualType` or `ConstId`, never by another item's `NodeId` (Part D).
7. **Keys stay within 32 bytes** (§A10.1). The largest planned keys fit:
   `analyze_body(ItemId, BodyId, CtxKey)` is 20 bytes, `default_arg(DeclId, u32, CtxKey)` 24,
   `resolve(u32, DeclListId, SeqId, SeqId, CtxKey)` 28.

## 3. Syntax and semantics

### 3.1 The splitter yields skeletons, not trees

The token-level splitter (§B1.2, #18) records per item a *skeleton*: the item's kind, its declared
names in token order (each with its ordinal, its token offset, whether it is a type, template or
namespace name or, for a using-declaration, unknown until lookup, and whether it is a definition), the spelled qualifier, the template depth, the `BodyId`, the
nested member items of a class, and the length of the declaration part. The syntactic indexes of
§B1.3 are built from skeletons.

*Reason:* at namespace and class scope every item is a declaration, so its declarator-ids can be
found without lookup, including constructors ([class.ctor.general]). A full parse cannot: C++
cannot be parsed without knowing which names are types and templates.

*Rejected:* a lookup-free parse tree from `region_items`. `a*b;`, `T(x);` and `f<a>(b)` are
ambiguous without lookup.

### 3.2 The parser analyses as it parses

The parser runs inside the contextual cells `item_sema(item, ctx)` (§A10.3's `ItemAnalysis`) and
`analyze_body(item, body, ctx)`, and asks its lookups through `cx`. It produces one analysed tree
per part of an item. There is no separate syntax-tree cell.

*Reason:* `a.b<c>(d)` is a template-id only if lookup of `b` in the class of `a` finds a template
([basic.lookup.qual.general], [temp.names]); `(a)(b)` is a cast only if `a` is a type
([dcl.ambig.res]). The language's `typename` and `template` disambiguators exist precisely
because the dependent cases cannot be parsed otherwise.

*Rejected:*
- a syntax-tree cell feeding a semantic cell: it cannot be produced without types;
- semantic annotations keyed by syntax node: shared nodes have many occurrences, so a node is not
  a key;
- an ambiguity forest resolved later: two parsers, and `a.b<` still needs the type of `a`.

### 3.3 One node family, two groups of kinds

All trees are made of one closed list of node kinds, `kinds<…>`:

- **Structure nodes** (statements, declarators, specifiers, written type-ids, attributes) record
  what was written and what it introduces.
- **Expression nodes** carry a sugared type (§5.3), a value category and their resolution: the
  `EntityId` they name, a `DeclId` where a property of the particular declaration matters (default
  arguments, [dcl.fct.default]), or a `ConstId`.

Implicit operations are nodes of width zero: conversions, temporary materialization, filled-in
default arguments, destructor calls, implicit `this`. Dependent constructs are expression nodes of
dependent kinds (§6), and instantiation maps a tree of this family to another tree of it.

*Reason:* the IR generator and the constant evaluator need implicit operations explicit;
templates need a symbolic form; one family means one interner, one dispatch table and one red
view.

*Rejected:* separate syntax and semantic families (two of everything, and a mapping between them
that would have to be stored); types on structure nodes only.

### 3.4 Extent

A tree covers every token of its part, but punctuation and keywords implied by a node's kind are
not stored: the token stream itself is kept, content-addressed. Parentheses are a node kind, since
they change meaning (`decltype((x))`, `(f)(x)`). Offsets inside a body tree are relative to the
body, so identical bodies are one tree. A `Loc` inside a body is item-relative (declaration-part
length plus body offset), and the cell that owns it is keyed by that body's `BodyId` (§8).

*Rejected:* trivia and punctuation nodes. Nothing needs them that the token stream does not give.

### 3.5 The red view

A red view is a cursor on the stack: interner, `NodeId`, item-relative offset and the parent
cursor. Analysis, IR generation and LSP descent all walk with it. It is never interned or stored,
and a node's parent is only ever the cursor chain of the current walk (spike, §10).

### 3.6 Record shapes

```cpp
struct ItemSema { NodeId tree; SeqId<DeclSema> decls; };                 // item_sema(item, ctx)
struct DeclSema { EntityId entity; QualType type; u8 kind, linkage, storage, access; u16 flags;
                  SeqId<AttrId> attributes; NodeId constraint;
                  NodeId contracts;             // non-members only, this declaration's own (§7.5)
                  SeqId<ParamSema> params; };   // flags include explicit_object (§5.4)
struct ClassDef { SeqId<BaseSpec> bases; SeqId<MemberRec> members; SeqId<MemberRec> friends;
                  SeqId<DeclSema> synthesized;  // class-1 declarations (§4.1)
                  SeqId<EntityId> nested; EntityId injected_class_name; u16 flags; };
struct BodySema { NodeId tree; SeqId<LocalRec> locals; };                // analyze_body
```

- `decl_sema(d, ctx)` stays a cell (§B1.3). For a written declaration it fetches `item_sema` and
  returns `decls[d.ord]`. For a synthesized one it fetches `class_def(C)` and returns its
  `synthesized` entry: whether an implicit copy constructor is deleted or trivial depends on the
  members. There is no cycle, because `class_def` fetches only written members.
- Lists that hold `NodeId`s (`SeqId<DeclSema>`, `SeqId<LocalRec>`) are interned with their tree
  when the frame commits, not eagerly (§9.3).
- A parameter's default argument is in `ParamSema` for a non-member function; for a member it is
  the separate query `default_arg(p, ctx)` in the complete-class context (§B8). Default arguments
  of a function template specialization come from `default_arg` keyed by the specialization's
  `EntityId` (§6.4).
- `class_def(C, ctx)` hops to the class item, reads its head and bases from `ItemSema`, and
  fetches `decl_sema` of each member.
- **What `function_ir(F, ctx)` reads:** `BodySema.tree` through the hop of §B1.4, `decl_sema(F)`,
  `layout` and `vtable` of the classes it touches, and constant values through `demand`. The
  hashed IR holds `EntityId`s and no offsets; each instruction's `Loc` goes to the debug side
  table (Part D on #3).

### 3.7 What the IR generator gets

The accepted optimization design asks the AST for nine things as explicit nodes or facts (its
§11). Each is met as follows; the two the decisions above left implicit, cleanups and
sequencing, were decided by the session.

1. **Conversions:** every implicit conversion, user-defined conversion call and standard
   conversion sequence is a width-0 node naming its function (§3.3).
2. **Temporaries:** materialization is a width-0 node carrying the storage duration, the
   destructor and the full-expression or reference that ends its lifetime, with lifetime
   extension resolved ([class.temporary]).
3. **Cleanups:** every scope-exit edge (normal exit, `return`, `break`, `continue`, `goto`, the
   exceptional edge) carries its ordered list of destructor calls, partially constructed objects
   included ([except.ctor]). It is a `SeqId` on the exiting statement node, computed by
   `analyze_body`.
4. **Sequencing:** each expression kind declares the sequencing of its operands by an annotation
   on its members (`[[=sequenced]]`, `[[=indeterminately_sequenced]]`, `[[=unsequenced]]`,
   [intro.execution]), read by reflection like the rest of the schema. The rule belongs to the
   kind, so shared nodes need no per-occurrence data, and the IR generator reads it rather than
   re-deriving it.
5. **Objects:** value categories and the implicit object parameter are resolved; `this`
   adjustments and derived-to-base and base-to-derived steps are conversion nodes naming the
   classes.
6. **Initialization** is decided in the tree: aggregate element order, default member
   initializers substituted, zero, value and default initialization explicit, constant versus
   dynamic initialization per variable ([basic.start.static]), and the erroneous or indeterminate
   state of each automatic variable ([dcl.init.general]).
7. **Function facts** are in `DeclSema`: constexpr and consteval, `noexcept` as a fact, the
   coroutine transformation with its promise calls explicit ([dcl.fct.def.coroutine]); `if
   consteval` is its own statement kind, and expressions carry a manifestly-constant-evaluated
   flag.
8. **Reflections** are `VReflect` constants (§7.1) whose components map to §B7's queries.
9. **References** are `EntityId`s and canonical `QualType`s (§5.2). String literals are the one
   exception: they are identified by their `VString` constant, not by an entity, since
   [lex.string] leaves their distinctness unspecified and content identity is TU-free.

## 4. Identities

### 4.1 Declaration ids

`DeclId = (ItemId, DeclOrd)`, where `DeclOrd` is a 4-bit class and a 28-bit number:

- **Syntactic** (class 0): the n-th name the item declares, in token order. The head of a class,
  enum or namespace is 0 and enumerators follow. In `struct { … } s;` the unnamed class is 0 and
  `s` is 1. A structured binding declaration declares its hidden variable first, then each name.
- **Synthesized** (class 1), in fixed slots: default constructor, copy and move constructors, copy
  and move assignment, destructor, injected-class-name, the copy deduction candidate, then implicit
  deduction guides by constructor ordinal and, for inherited constructors, by `(using-declaration
  ordinal, base constructor ordinal)` ([over.match.class.deduct]). Their records live in
  `ClassDef` (§3.6).
- **Injected** (class 2): the n-th declaration produced by a consteval-block item (§B7,
  `item_effects`).

Function parameters and template parameters are positional, `(DeclId, index)`, and template
parameters are matched by depth and index ([temp.param], [temp.over.link]).

*Reason:* 8 bytes with no interner, and exactly as stable as the item (§B1.2).

*Rejected:* one item per declarator (the splitter's item rule is fixed by §B1.2); interned
declaration ids (an interner for no gain).

### 4.2 Members share the class item's prefix

The members of a class are analysed at the prefix of the outermost enclosing *class* item (whose
position inside a namespace's item list is addressed by the prefix as usual). A
member's position among its siblings is its `DeclId`, and "the members before this one named `x`"
is the cheap, context-free query `class_decls_named_before(K, name, ord)` over the class item `K`'s
tokens. It is keyed by stable ids, so it appears in traces as it is.

Complete-class contexts ([class.mem.general]: member function bodies, default arguments, default
template arguments, noexcept-specifiers, function contract specifiers and default member
initializers, including those of nested classes; each is its own cell, `default_template_arg`
beside §B8's) are analysed at the
prefix **after** `K`: `K` is complete there, and nothing that follows `K` is visible.

*Reason:* `class_def(C)` needs `decl_sema(m)` of each member, so `decl_sema(m)` must never reach
`class_def(C)` (Part D, §A2.4). A syntactic per-name index gives the member-ordering answer
without it, and prefixes stay at the count §A1.9 assumes.

*Rejected:* a prefix node per member (multiplies prefixes by members, for answers a per-class
index gives directly).

**Addition to the engine:** the prefix after `K` cannot be written as "the prefix before `K`'s
successor": when `K` is the last item of a header, its successor differs between TUs. The engine
needs `cx.prefix_after_item(K)`, recorded in traces as `rel_after(◇.prefix, K)`, next to
`prefix_of`. It is additive to §A1.9 (§11).

### 4.3 Entity ids

The records of §B1.5, made concrete:

- namespace, class, enumeration, variable: `(scope, name, module)`;
- function: `(scope, name, module, canonical type, template head, trailing requires-clause)`, where
  the template head is the template parameter list and its requires-clause and both clauses are
  `VDep*` constants or `VNone` ([temp.over.link] makes them part of correspondence);
- specialization: `(template, canonical arguments)`;
- internal linkage or unnamed: `(scope, name or none, first DeclId)`;
- **local** entities (block-scope classes, lambdas' closure types, block-scope statics):
  `(enclosing entity, kind, name or none, ordinal among same-kind, same-name locals in token
  order)`. This follows the Itanium ABI's discriminators, so a local entity's mangled name is a
  function of its id. A lambda in a declaration part takes the entity of the enclosing
  declaration as its enclosing entity;
- objects that are not declared entities but can be the base of a constant pointer
  ([expr.const]): a temporary lifetime-extended by a static-storage reference
  `(owner entity, ordinal)`, and a template parameter object `(canonical type, value)`
  ([temp.param]); `define_static_array` and `define_static_string` produce template parameter
  objects.

`module` is the named module the declaration is attached to, or none for the global module
([basic.link], [module.unit]). It is a recorded question of the item's own prefix,
`module_of(prefix)`. A function with C language linkage uses `::` as its scope, since such
functions in different namespaces are one function ([dcl.link]).

A block-scope `extern` declaration gets the namespace-scope entity's id ([basic.link]). An
implicit special member gets the id a user-declared one would have. `entity_of(decl, ctx)` is a
contextual cell, and its result is context-free.

### 4.4 No declaration ids inside bodies

Block-scope declarations, lambdas, local classes and bindings are plain data of the body tree.
Those that can be named from outside the body get an `EntityId` (§4.3); tooling locates them by
`Loc`.

*Reason:* a `DeclId` inside a body would need the `BodyId` to be unambiguous (one item has as many
bodies as there are expansions of its macros, §B1.2), and with the `BodyId` it would change on
every body edit.

### 4.5 What survives an edit

| Identity | Survives | Changes when |
| --- | --- | --- |
| `ItemId` | body edits, edits to other items, edits above it | its own declaration part, scope path or file changes, or an identical earlier sibling appears or goes |
| `DeclId` | as its item | as its item |
| `EntityId` | everything but its scope, name or signature | those change; a local id shifts when an earlier local of the same kind appears; an internal-linkage id follows its first declaration |
| `Loc` | everything that does not move tokens before it in its item | tokens before it in its item move |

### 4.6 What an LSP client holds

The client holds `(file, line, column)` and the server resolves it again on every request: §8.5
maps it to `(item, offset)`, the red view descends to the node, and the node gives an `EntityId`
or `DeclId`. "Go to definition" asks `syntactic_definition(scope, name, END)` and matches by
`decl_sema`; "find references" is a derived query keyed by `EntityId`. Where a client needs a
stable token, it gets the 128-bit fingerprint of an `EntityId` or `DeclId`, never a handle.

*Rejected:* handing out handles or node ids: compaction renumbers them, and restarts invalidate
them.

## 5. Types and constants

### 5.1 Type kinds

One closed list of type kinds lives in the `types` interner, in two classes.

**Canonical kinds:**

```cpp
TBuiltin{Builtin b}         // void, nullptr_t, bool, character, integer and floating types,
                            // meta_info, and the markers dependent, overload_set, bound_member
TPointer{QualType}  TLRef{QualType}  TRRef{QualType}  TMemberPtr{QualType cls; QualType member}
TArray{QualType elem; ConstId bound}                  // an integer, none (unknown bound) or dependent
TFunction{QualType ret; TypeListId params; ConstId noexcept_; u8 variadic:1, cv:2, refq:2}
TClass{EntityId}  TEnum{EntityId}                     // X<T> with dependent arguments is TClass of (X, [T])
TTemplateParam{u16 depth, index; u8 pack}
TDepName{QualType qualifier; NameId name; TemplateArgs args}
TDecltype{ConstId expr; u8 id_or_member}  TSplice{ConstId refl}  TPackExpansion{QualType pattern}
TPackIndex{QualType pack; ConstId index}  TAuto{u8 kind; EntityId concept; TemplateArgs cargs}
TDeducedClass{EntityId tmpl}
```

**Sugar kinds**, each with one child marked `[[=canonical]]`, the type it spells: `STypedef{EntityId
alias; TemplateArgs args}`, `SElaborated{u8 keyword}`, `SQualified{ScopePathId}`,
`SSpelledArgs{TemplateArgs written}`, `SDecltype`, `SAuto{u8 kind}`, `SSplice`.

*Reason:* one list gets fingerprint and equality from reflection like the AST does (§10), and
sugar as nodes keeps `QualType` at 4 bytes.

*Rejected:* a sugar table keyed by node (blind to which occurrence is meant); sugar bits in
`QualType` (cv needs the 3 bits).

### 5.2 Canonical form is a parallel array

`types` keeps, beside its fingerprint array (§9.4), an array `canon[node]` filled when a node is
interned. An entry is a `QualType`, so the cv a typedef adds survives (`typedef const int A;`).
Children are interned first, so `canon(TPointer{p})` is `intern(TPointer{canonical(p)})`, a sugar
node's entry is its child's canonical form, and every other node's is itself. Then `canonical(q)`
is `with_cv(canon[q.node], q.cv)`, where `with_cv` merges the two cvs, pushes cv into array elements
([basic.type.qualifier]) and drops it on references ([dcl.ref]) and function types.

Invariants: a canonical `QualType` has no cv on an array, reference or function node, and a
canonical node's children are canonical. `canon[]` is derived, like the fingerprint, and is not
part of a node's identity (§2, invariant 4).

*Reason:* expressions carry sugared types (§5.3), so every semantic comparison canonicalizes, and
must do it in O(1). The spike's rebuild is O(size of the type).

*Rejected:* a canonical bit with a rebuild on demand; a cache field in the node (breaks
invariant 4).

### 5.3 Expressions carry the sugared type

Expression nodes carry the type as written or deduced, sugar included, so diagnostics and hover
say `std::string`. Everything semantic uses `canon[]`: overload-resolution keys (§B5), entity ids
(§4.3), the IR. Tree equality and fingerprints are therefore over sugar.

*Rejected:* a canonical and a sugared type in every expression node (doubles the payload, §9.6);
canonical only (diagnostics lose the user's spelling). This revises the first brief's "canonical
`QualType`", which the session queried on review.

### 5.4 Canonicalizations done at construction

- The parameter adjustments of [dcl.fct] (arrays and functions decay, top-level cv is dropped)
  when a `TFunction` is made from declarators; the declarator tree keeps the written type-id.
- Reference collapsing ([dcl.ref]) in the constructor of reference types.
- cv applied through a typedef to a reference or function type is dropped by `with_cv`.
- A non-dependent `decltype(e)` is resolved at construction and kept as `SDecltype` sugar.
  `TDepName`, `TDecltype`, `TSplice` and `TPackIndex` exist only while dependent.
- An explicit object parameter ([dcl.fct]) is parameter 0 of the `TFunction` and is flagged in
  `DeclSema`, not in the type, so `&S::f` is an ordinary pointer to function.

Types mention entities only by `EntityId`, whose internal-linkage and local records are TU-free
(§4.3), so types are TU-free with nothing added.

### 5.5 Constants

Constant values live in the `values` interner as `ConstId`s:

- `VInt{QualType; u64 lo, hi}`, `VFloat{QualType; u64 lo, hi}` (wide enough for 80- and 128-bit
  formats), `VNullptr`, `VLvalue{ConstId base; SeqId<u32> path; i64 offset}`, `VString{QualType;
  SeqId<u8>}`, `VMemberPtr`, `VAggregate{QualType; SeqId<ConstId>}`, `VUnion{QualType; u32 active;
  ConstId value}`, `VReflect` (§7.1), `VNone`, and `VNotConstant{SeqId<Note>}`, the failure value
  of §A1.7;
- the base of a `VLvalue` is `VEntity{EntityId}` (a declared object or function), a `VString`
  (a string literal object), or a `VEntity` of a lifetime-extended temporary or a template
  parameter object, whose entity records §4.3 defines ([expr.const]);
- the **dependent family** `VDep*`: `VDepParam{depth, index}` (a template parameter),
  `VDepFnParam{depth, index}` (a function parameter named in a trailing return type or
  `noexcept`, positional, as the Itanium ABI mangles it), `VDepOp{op; TemplateArgs operands}` (so
  an operand may be a type: casts, `T{}`, `alignof(T)`, `new T`, `typeid(T)`),
  `VDepCall{callee; SeqId<ConstId>}`, `VDepName{QualType qualifier; NameId; TemplateArgs}`,
  `VDepMember{ConstId object; NameId; TemplateArgs; arrow}`, `VDepRequires{TypeListId params;
  SeqId<Requirement>}` ([expr.prim.req]), `VDepSizeof{QualType}`, `VDepFold`,
  `VDepExpansion{ConstId pattern}`, `VDepPackIndex`, `VDepSplice`, `VDepReflect`.

A function's own type never contains its own `EntityId`; parameters it mentions are
`VDepFnParam`, so `decltype(x + y)` and `decltype(a + b)` in two declarations of one template are
equal.

A dependent expression in a **canonical position** (a template argument, an array bound, a
noexcept operand, a decltype operand, a pack index, a constraint's parameter mapping) is stored in
its `VDep*` form: non-dependent names bound, parentheses and sugar dropped. Equality is the
equivalence of [temp.over.link], modulo parentheses; functional equivalence is not detected, which
the clause makes ill-formed, no diagnostic required.

*Reason:* `values` is interned eagerly and is fit for keys. A dependent value is a value with free
parameters, substitution maps `ConstId` to `ConstId`, and no `NodeId` enters a type or a key.

*Rejected:* a `NodeId` inside a type (would force eager interning of speculative trees, §9.3, and
breaks invariant 6); token sequences (parameter renaming, and the non-dependent names would stay
unbound).

### 5.6 Template arguments

`TemplateArg{u8 kind; u32 payload}` (8 bytes), with kind type, value, template, template parameter
or pack expansion. `TemplateArgs = SeqId<TemplateArg>` is one level, with packs flattened and
values converted ([temp.arg.nontype]) or in `VDep*` form. Conversion canonicalizes every type
inside a value (a `VReflect` of a type keeps an `STypedef` top node, as §7.1 compares), so
template-argument-equivalent values ([temp.type]) are one `ConstId`: `X<A(1)>` and `X<1>` are one
specialization. `ArgLevels = SeqId<TemplateArgs>` holds
the levels outermost first; the levels of `X<int>::f<char>` come from the entity's scope chain
(§4.3).

## 6. Templates

### 6.1 A template is analysed by the ordinary cells

There is no separate `analyze_template` kind: `item_sema` and `analyze_body` on a template item
*are* §B6.1. Non-dependent parts are bound, as ordinary nodes. Dependent parts are the same
kinds, with the type `TBuiltin{dependent}` (or the type [temp.dep.expr] fixes, such as `size_t`
for `sizeof(T)`) and no resolution. Three kinds are added for what only templates have:

- `DepName{NameId; TemplateArgs explicit_args; DeclListId definition_lookup}` is the callee of a
  dependent call and holds §B6.1's definition-context lookup set. An operator gets one too, of
  width zero: `a + b` has a `DepName{operator+, …}` child.
- `DepMember{Child object; NameId; TemplateArgs; arrow}` for dependent member access.
- `DepQualified{QualType scope; NameId; TemplateArgs}` for dependent qualified names.

Trees hold `VDep*` constants only in canonical positions (`TemplateId{EntityId; TemplateArgs}`,
type-id nodes).

*Rejected:* a separate family of dependent trees (§3.3).

### 6.2 The current instantiation

Inside `X`, `X<T>` is `TClass` of `(X, [T])`. The parser recognizes the current instantiation by
comparing with the enclosing template's entity ([temp.dep.type]). `class_def` of a dependent
entity is a contract violation.

### 6.3 Substitution is a function, and its lookups are the cells

```cpp
substitute(QualType | ConstId | TemplateArgs, ArgLevels, cx)
  -> Subst{bool ok; u8 failure_kind; QualType a; NameId n; Loc at /* in the pattern */};
```

`substitute` is a transparent function (§A1.1), not a query. Structural cases rebuild through
`types` and `values`. `TDepName` fetches `member_lookup` and `decl_sema`, and may demand
`instantiate_class`. `TDecltype` and `VDep*` operands are instantiated into a tree in the frame
arena, then typed and evaluated within the frame (§B7). An unqualified `VDepName` callee has no
stored lookup set (its equality is by name), so substitution performs the definition-context
lookup of [temp.dep.candidate] itself: `visible_decls` at `cx.prefix_of(pattern item)`, recorded
as `rel(◇.prefix, I)`, plus ADL at its own prefix. Failure is a value (§A1.7): `deduce`,
`satisfies` and `resolve` decide what it means.

*Rejected:* a cell per substituted type: millions of cells for work that is mostly structural
rebuilding, below the granularity of §B0.5. If measurement says otherwise, a
`substitute_type(QualType, ArgLevels, ctx)` cell fits in 20 bytes and callers do not change (§12).

### 6.4 Instantiation produces a fresh tree with the pattern's offsets

`instantiate_fn(spec, ctx)` returns `FnInst{BodySema body; NodeId contracts}`. Default arguments
are not part of it: `instantiate_fn` runs at END (§B6.2), but a call fills in its default
arguments at its own prefix. So `default_arg(EntityId spec, u32 index, ctx)` is fetched by the
caller for every function template specialization, and `default_arg` and `contract_specs` for a
member of a class template specialization likewise take the specialization's `EntityId` as key
data. `instantiate_class(spec, ctx)` returns a
`ClassDef` whose members pair the pattern's declaration with the instantiated entity:
`MemberRec{DeclId pattern; EntityId instantiated; QualType substituted; …}`. A member template of
a specialization is `(DeclId pattern, TemplateArgs outer)`, and it is instantiated at all levels at
once (§5.6), with no intermediate tree.

The instantiated tree keeps the pattern's offsets (`Child.at` unchanged), so the red view, the IR
generator and LSP descent work on it unmodified, and instantiation diagnostics and DWARF rows
point into the template's tokens.

*Rejected:* applying the substitution lazily through an environment (every consumer would thread
it, and equal results would not cut off); re-parsing the tokens under the bindings (loses the
phase-one bindings of [temp.res]).

### 6.5 Instantiated declarations need no new declaration ids

An instantiated declaration is its `EntityId` (§4.3) plus the pattern's `DeclId`. `decl_sema(d)`
is the pattern's record; the specialization's record lives in `instantiate_class(spec).members`.

A pattern `DeclId` alone does not identify an instantiated candidate: the hidden friend `f` of
`X<int>` and of `X<long>` share one. So lookup results that reach members or friends of a
specialization, `ClassDef.members` and `ClassDef.friends`, carry `(DeclId pattern, EntityId
instantiated)`, as `MemberRec` does; `resolve`'s candidate set (§B5) is a set of such pairs, and
`function_type` reads the instantiated entity's type. This extends §B3's "lookup results are lists
of declaration ids" without changing it for non-template code, where the entity is not needed.

## 7. C++26 constructs

### 7.1 Reflection values

A `std::meta::info` value is a constant `VReflect{u8 kind; u32 a; u32 b}` in `values`, of type
`TBuiltin{meta_info}`. Its kinds: null; type (a sugared `QualType`); entity (an `EntityId`:
namespace, function, variable, class, enumeration, enumerator, template, concept, data member);
parameter `(function EntityId, index)`; base `(class EntityId, index)`; value (a `ConstId`);
object (a `VLvalue`); annotation `(EntityId, ordinal)`; data member specification (a
`VAggregate`). `^^T` with `T` dependent is `VDepReflect`.

Equality ([expr.eq]) compares kind and components. For a type, a reflection whose top node is
`STypedef` compares by alias identity (entity, arguments, cv) and any other by `canon[]`, so
`^^A != ^^int` while `^^A* == ^^int*`. The fingerprint is Merkle through handles, and every
component is TU-free, so reflections in shared constant values are TU-free.

*Rejected:* a separate reflection interner (`values` suffices); reflecting `DeclId`s
(redeclarations of one entity must reflect equal).

**Block-scope entities** (a local variable, a parameter, a local structured binding or class)
have no `DeclId` (§4.4). Reflecting one interns, at that moment, a local `EntityId` of the form of
§4.3, `(enclosing entity, kind, ordinal)`, extended with the kinds variable, parameter and binding;
the IR keeps referring to locals by `LocalRec` index. `^^p` and `parameters_of(^^f)[i]` both give
`parameter(f, i)`. Identity is content, so two evaluations of `^^x` in one function give one
`ConstId`; locals of a specialization use the specialization's entity, and inside expansion
statements the order of §7.4. (This answered a gap the session raised on review.)

### 7.2 Splices

A non-dependent `[: r :]` evaluates `r` within the frame (§B7). The result is the spliced type,
kept as `SSplice` sugar, or a `Splice{Child r; ConstId value}` node resolved to an `EntityId` or
`ConstId`. A dependent splice is `TSplice{VDep*}` as a type, `TDepName` over a `TSplice` for
`typename [:r:]::x` and `[:r:]<A>`, an expression `Splice` of dependent type in trees, and
`VDepSplice` in canonical positions.

### 7.3 `define_aggregate` and consteval blocks

§B7's `item_effects(J, ctx)` returns `SeqId<Effect>`, with `Effect = CompleteClass{EntityId;
SeqId<MemberSpec{QualType; NameId; ConstId width, align; u8 flags}>}`. Each produced member gets
`DeclId (J, injected, n)` (§4.1), a `DeclSema` synthesized from its specification with no tree, and
the `Loc` of the call in `J`. `class_def(S)` is built from it when `injected_definitions` names
`S`.

### 7.4 Expansion statements

`ExpansionStmt{Child decl; Child range; ChildList body}` (P1306, [stmt.expand]). If the range is
dependent, `body` is the **pattern**: a dependent tree in which the expansion variable is a
`DepName`. Otherwise `body` holds the **instances**, one fresh tree per element (§6.4), each at
offset 0 of the pattern's tokens. Never both. The IR sees instances only. Local entities inside
the instances are numbered in expanded traversal order (§4.3; §12 has the mangling question).

### 7.5 Contracts

Preconditions and postconditions are declaration data, not part of the type
([dcl.contract.func]).

- For a non-member function, `PreAssert` and `PostAssert{NameId result; Child predicate}` are
  structure nodes in the declaration's `ItemSema.tree`.
- For a member function they are analysed in the complete-class context, by a new per-part cell
  `contract_specs(f, ctx)` beside §B8's `default_arg`, `default_member_init` and `noexcept_spec`.
- `contract_assert` in a body is the statement kind `ContractAssert{Child predicate}`.
- For a non-member, `DeclSema.contracts` holds the declaration's *own* tree, with a flag saying
  whether it agrees with the first declaration's ([dcl.contract.func]); it never holds another
  item's node. A member's contracts are only in `contract_specs`, so `decl_sema` never reaches a
  complete-class cell (§A2.4).
- A function template specialization's contracts are instantiated with it (`FnInst`, §6.4).

The evaluation semantic is configuration (the prefix's root node), read by `function_ir`; trees
record only what was written.

*Rejected:* copying preconditions and postconditions into the body tree. A body would then change
when its declaration's contracts change, and identical bodies of different declarations would no
longer be shared.

### 7.6 Pack indexing and binding packs

`T...[I]` is `TPackIndex` while dependent and otherwise the selected element, with no sugar.
`e...[I]` is `PackIndex{Child pack; Child index}`, `VDepPackIndex` in canonical positions, and is
replaced at substitution by the selected element's node ([expr.prim.pack.index]). A structured
binding pack (P1061) with a dependent initializer is a pack `LocalRec` used through
`VDepExpansion`; with a non-dependent one it is expanded at once into instances, as in §7.4.

## 8. Source locations

### 8.1 Tokens carry no location; the region carries a parallel table

The tokens of an item (`TokSeqId`, `BodyId`) are `Tok{u8 kind; u8 flags; u32 payload}`, so an item's
identity ignores whitespace and provenance (§B1.2). Beside its token stream, the region that `pp`
produces (§B9) holds a parallel table:

```cpp
struct TokLoc   { u16 file; u32 expansion; u32 offset; };   // byte offset of the spelling in `file`
struct Expansion{ u8 kind;      // object, function, argument, stringize, paste, va_opt, pragma,
                                // builtin, embed
                  NameId macro; u32 parent; u16 site_file; u32 site_offset;
                  u16 definition_file; u32 definition_offset; u32 operand_a, operand_b; };
```

`file` indexes the region's file table: its own file, files of macro definitions, files included
inside an item, `<command line>` and `<built-in>`. `expansion` is the innermost expansion that
produced the token, 0 for none. `region_items` flattens a child region that falls inside an item
into the item's tokens, so an item may span files (`enum E { #include "values.h" };`), and their
`TokLoc`s name the child's file.

The location table is part of the region's value and therefore of the tree id, which amends
§B1.1's list of what the tree id covers. A prefix node's leaf handle is the hash-consed prefix of
the region's `(tokens, TokLoc)` stream up to its item `k`, so an edit after item `k` leaves the
prefixes before it unchanged (§A1.9), while the syntactic indexes (§B1.3)
and item ids are keyed by the region's location-free `items` sub-handle. A whitespace edit in a
header therefore recomputes no index and changes no item id. It does give new prefixes to the
items after it in every TU that includes the header, whose selection cells re-walk their tries at
one O(1) question per trace element and land on their old leaves, as §B11 describes for body
edits. Only cells that asked for a location (§8.7) compute again. (Settled on review: the session
asked whether the table belongs in the tree id. Keeping it out would make §8.7's question an
untracked read.)

*Rejected:* locations inside `Tok` (reformatting would change every item id); per-TU location
tables.

### 8.2 Offsets count tokens

A `Loc`'s offset counts tokens, as `Child.at` does, so reformatting moves no `Loc` (§4.5). A
position inside a token (a bad escape in a literal) travels in the diagnostic's arguments as a
column within the token. A range is `Range{Loc begin; u32 end}`, with `end` an exclusive token
index, rendered from the start of `begin` to the end of token `end - 1`. DWARF needs the line and
column of statements and calls only, which tokens give.

*Rejected:* character offsets: a reformatting would move them, and trees count tokens.

### 8.3 Rendering

`render(Loc, TU)`:

1. `tu_item_index(TreeId)` maps each `ItemId` to its region occurrence and its first token index,
   from the TU's tree. An item occurs once in a TU and has one body there: the salt of §B1.2 counts
   earlier occurrences of the same *item id* in the prefix (not only of an identical region), so
   two occurrences with different bodies, which only an ill-formed redefinition produces, still
   get distinct ids, so a `Loc` inside a body needs no `BodyId`.
2. The token index is the item's first token plus the `Loc`'s offset.
3. Its `TokLoc` gives the file and byte offset.
4. `line_table(content)`, a memoized cell keyed by file content, gives line and column by binary search. The
   region's `#line` table gives presumed values (§8.6).
5. The macro backtrace follows `Expansion.parent`, rendering each site in its `site_file` (for a
   nested expansion, the outer macro's definition file) and each definition in its file.
6. The include chain is the occurrence's path in the TU's tree, each include directive rendered
   the same way.

It costs one hash probe, a binary search and the depth of the macro and include nesting.
`render` is a function called per diagnostic or DWARF row, not a cell; `tu_item_index(TreeId)`
and `line_table` are memoized, content-keyed cells, so its reads are recorded. For a module unit, an importer resolves an `ItemId` that is not in its own
index through the indexes of the imported module's TUs (`module_interface(M)` names its TU), and
the chain reads "imported from M".

### 8.4 Debug information

As the accepted optimization design has it (its §2.4): `function_ir`'s value contains no `Loc`, and
`function_debug(F, ctx)` is a separate value mapping IR node ids to `Loc`s. Every transformation
carries an origin map to `(function entity, node id)` pairs, so `tu(TU)` composes the maps,
looks up each origin's `Loc` and renders the line table. A token produced by a macro is attributed to the **outermost expansion site**, since
debuggers step through the user's file; `is_stmt` marks statement starts. A function from a
header emitted by several TUs renders to the same file and line in each; instantiations point at
the template's lines (§6.4); inlined code keeps its own `ItemId`.

### 8.5 LSP: from a cursor to an item

A cursor `(file, line, column)` becomes a byte offset through `line_table`. The TU is the first, in
command-line order, whose tree contains the file; open documents are kept in memory (§A6.2). In
each occurrence of the file in that tree, a binary search over the `TokLoc`s that name the file
(macro argument tokens included) finds the token, and the item whose token range holds it gives
`(ItemId, offset)`. A cursor on a macro *name* finds no produced token that spells it; it maps to
the expansion whose site covers the offset, at its first token. A cursor in an inactive `#if`
branch or a `#define` body is in no item, and only macro information is offered. A file that
preprocesses differently per TU is shown in the chosen TU's view; the client can switch TU.

### 8.6 Edge cases

- **`#line`** ([cpp.line]): `TokLoc` stays physical. The region keeps
  `line_directives = SeqId<{u32 from_token; NameId file; i32 delta}>`, applied at rendering and by
  `pp` for `__LINE__` and `__FILE__` (§B9).
- **`__LINE__`, `__COUNTER__`:** the produced token's expansion has kind builtin, with the site at
  the macro token.
- **`#embed`** ([cpp.embed]): *one* token carrying the resource's bytes, expanded by the parser
  where an initializer or expression list is accepted. `prefix`, `suffix` and `if_empty` yield
  ordinary tokens around it. Its `TokLoc` is the directive, with an expansion of kind embed whose
  definition file is the resource.
- **`##`** ([cpp.concat]): the pasted token's spelling is the first operand's, with an expansion
  of kind paste recording both operands' locations ("pasted from").
- **`#pragma` and `_Pragma`** ([cpp.pragma.op]): directive items in the item list, like include
  items; one inside an item is hoisted before it (§B9's rule), with an expansion pointing at the
  macro. Diagnostic pragmas are applied by `tu_diagnostics` by rendered position.

### 8.7 Positions as constant values

`std::source_location::current()` ([support.srcloc]) and P2996's `source_location_of(r)` are
constant-evaluation results that hold a file name, line and column, and they change when lines
move. They are computed from a recorded question: §B1.3's cheap `item_location` becomes

```cpp
token_location(ItemId I, u32 offset, [[=context]] Prefix at)
  -> {NameId presumed_file; u32 line, column};                     // 12-byte key and value
```

It finds `I`'s region among the prefix's levels (`prefix_item_lists`), reads the token's `TokLoc`,
then `line_table`, the region's `#line` table and its presumed file name. The presumed name is the
path as spelled in the `#include`, which §B9 already makes part of `pp`'s result, so a header
reached by two spellings is two regions, as it already is when it uses `__FILE__`.

- `source_location::current()` asks it at the use site's `(item, offset)` and its own prefix;
  `function_name()` is a function of the enclosing `EntityId`.
- `source_location_of(r)` finds the entity's first declaration by ordinary recorded lookup, takes
  the offset of the declared name (the skeleton records one per name, §3.1) or the local's
  `LocalRec` offset, and asks `token_location`.

The value is a `VAggregate` holding a `VLvalue` of a `VString`. A whitespace edit above the use site
changes the prefix (§8.1); the re-walk fails at `token_location` in exactly the cells that asked
it, and only they compute again, because their output really changed. (Settled on review, like
§8.1's paragraph on identity.)

## 9. Memory, arenas and interning

1. **One interner per handle type,** process-wide: `names`, `tokens`, `items`, `entities`, `ast`,
   `types`, `values`, and `seq<T>` per element type. The engine owns prefixes, pin tables, chains,
   cells and diagnostics. Interners are append-only during a revision and compacted at
   quiescence (§A6.1). *Rejected:* one untyped interner (no typed handles, no per-type layout);
   per-TU interners (defeats sharing).
2. **Interning identity is the 128-bit fingerprint.** §A1.5 and §A1.9 already rest on it for
   cutoff and trace matching. Debug builds also compare structurally and fail a `contract_assert`
   on a collision.
3. **Trees are built in the frame arena and interned when the frame commits,** bottom-up, at
   `co_return`. Types, names, entities, constants and lists are interned eagerly, because they
   are used as keys, except lists that hold `NodeId`s, which are interned with their tree. *Reason:* backtracking parses, failed deductions, `requires` checks and
   SFINAE build expressions that are thrown away. Interning them eagerly would fill the interner
   with garbage, and a cancelled frame would leak into it (§A4.3). *Rejected:* interning node by
   node; interning a whole tree as one opaque blob (no shared subtrees, no per-node fingerprint).
4. **Per-kind dense pools.** Each node kind has a fixed-stride payload pool, with parallel arrays
   of fingerprints and widths, and each interner has one edge pool. The kind is in the handle, so
   nodes need no header, and dispatch is the generated table (§10). The pools are untyped byte
   pools indexed by kind, never a `std::tuple` over the kinds list (§10.6). *Rejected:* one byte
   arena per family with a kind header (sliding compaction, no column layout).
5. **What compaction needs** (§A6.2 step 3): every reference is a typed handle from
   `handles<…>`, so marking and rewriting are one reflection walk; `ChildList` and `SeqId` ranges
   are rewritten with their pools; hash tables are rebuilt from the fingerprint arrays.
   Fingerprints do not change, so cells, records and trie leaves are untouched.
6. **Budget.** A node's payload is at most 32 bytes, checked by a `static_assert` over
   `kinds<…>`. With its fingerprint, width and table entry that is about 64 bytes per distinct
   node, plus 8 per edge, or 10–15 KB per distinct body. A million distinct bodies would take
   10–15 GB, more than the budget, so body trees are evicted under §A6.2 step 1 (the fingerprint
   stays, and `function_ir` stays green), with open documents' trees kept. Types and entities are
   not evicted.
7. **Sharing across TUs follows from the identities:** item ids are TU-free, body trees are
   body-relative, types are canonical, entities structural. One `ast` interner serves every TU.

## 10. The spike

[`spikes/ast/`](../../spikes/ast/) is a throwaway probe of the mechanisms this design leans on,
built with GCC 16.2 `-freflection` (`make check`; ASan and UBSan builds run clean). It shows:

1. **A node schema is a list of plain aggregates.** Fingerprint, structural equality,
   hash-consing and child walking are written once, generically, by reflection
   (`nonstatic_data_members_of`, `define_static_array`, `template for`, splices). Annotations on
   members work. A function-scope `define_static_array` result must be `static constexpr` to be
   expanded over.
2. **Hash-consing with Merkle fingerprints, independent of interning order.** `x + 1` occurring
   twice in a body is one node. Two interners that intern the same tree in different orders give
   it different handles and equal fingerprints.
3. **The red view** computes the item-relative offset of each occurrence of a shared node.
4. **Type canonicalization.** `const A`, with `typedef int A[3]`, is the same canonical handle as
   `const int[3]`, with equal fingerprints across tables built in different orders.
5. **Dispatch.** The function-pointer table generated from the kinds list costs 6.5–6.9 ns per
   visit over 240 kinds and 4.4 ns over 6.
6. **Compile time.** 240 kinds, each with reflection-derived code and a dispatch entry, take about
   2 s of front-end time and 7.5 s for the whole `-O2` build; reflection and constant evaluation
   are about 1% of it. The first version kept the kinds in a `std::tuple<std::vector<Ts>...>`, and
   that alone cost over 50 s, nearly all of it overload resolution inside `std::get` on a
   240-element tuple. Hence §9.4.

## 11. Constraints on other designs

Posted as comments on the issues named.

**On the IR and optimizer (#3, accepted; for #23 and #24):**

1. `function_ir(F, ctx)` consumes a tree with no dependent kind and no `VDep*` constant, checked by
   a `contract_assert` on entry. For a specialization the tree is `instantiate_fn(F, ctx).body`. The
   IR never substitutes.
2. IR types are canonical `QualType`s (`canon[]`), never sugar. Layout and vtables come from
   `layout(C)` and `vtable(C)` by `EntityId`.
3. IR constants are `ConstId`s. A symbolic address is `VLvalue{base, path, offset}` (§5.5). The
   interpreter returns a `ConstId`, including `VNotConstant{notes}`. A `meta_info` value is an IR
   scalar legal only under constant evaluation; code generation meeting one is an internal error.
4. The IR carries no `Loc`; `function_debug(F, ctx)` maps its node ids to `Loc`s, and origin maps
   compose through every transformation, as the optimization design's §2.4 decides (§8.4).
5. Contracts: the IR generator reads `PreAssert`, `PostAssert` and `ContractAssert` and the
   configured evaluation semantic; a postcondition's result name binds to the return slot.
6. Expansion statements, pack indexing, splices and binding packs never reach the IR; it sees only
   instances and resolved nodes.
7. Body locals are referenced by `LocalRec` index (§4.4), lambdas and local classes by
   `EntityId`, internal-linkage entities by their TU-free `EntityId`, renamed at emission (§B1.5).
   The IR is TU-free.

**On contextual queries (#16):** add `cx.prefix_after_item(K)`, recorded in traces as
`rel_after(◇.prefix, K)`, for complete-class contexts (§4.2). It is additive to §A1.9 and
§A10.2.

**On the item splitter (#18):** the splitter produces the skeletons of §3.1 (declared names with
ordinals and flags, spelled qualifier, template depth, `BodyId`, member items, declaration-part
length), and `region_items` flattens child regions that fall inside an item (§8.1).

**On contextual queries (#16), an amendment to §B1.1:** the tree id covers the region's location
table; a prefix node's leaf handle is the hash-consed `(tokens, TokLoc)` prefix of the region up to
its item, while syntactic indexes are keyed by the region's location-free `items` sub-handle
(§8.1). `module_of(prefix)` is a new cheap question (§4.3). `item_location` becomes `token_location`
(§8.7).

## 12. Open items and follow-up issues

Each is settled by a measurement or a probe in the implementation issue named, not by more
design:

- **Node index width.** 24 bits allow 16 M distinct nodes per kind. Count distinct nodes per kind
  over libycxx and a large corpus once the parser exists; the escape is a 64-bit `NodeId` or a
  kind directory, with no format change since handles are never persisted.
- **Annotations on enumeration types** (handles annotated with their interner): §A10.4 probed
  enumerators, not enum types. Probe in the first AST commit; the fallback is a trait
  specialization marked `// BOOTSTRAP: annotations`.
- **`decl_sema` as its own cell** costs one cell per declared name for no extra cutoff. Keep it
  (§B1.3), measure on libycxx, and make it `transparent` over `item_sema` if cells dominate.
- **`substitute` as a function** (§6.3): measure on libycxx; if resolving `TDepName` dominates,
  make `substitute_type` a cell.
- **Local entity numbering inside `template for` instances** (§7.4): the Itanium ABI has no rule
  yet. Run GCC 16 as a black-box oracle on a lambda inside an expansion statement and match its
  mangling if it has one.
- **`std::meta::info` and code generation:** whether the pinned draft makes `meta_info`
  consteval-only (§11, item 3); read [basic.types.general] and [meta.reflection] at the pinned
  commit.
- **Reflection equality of cv-qualified aliases** (`^^const A`, §7.1): confirm against [expr.eq]
  at the pinned commit.
- **An include inside an item** (§8.1 and Part E of the query-engine design): confirm with the
  preprocessor work that the included tokens join the enclosing item while the include's tree is
  placed before the item in the prefix.
- **Reflecting structured bindings and parameters** (§7.1): whether the pinned draft lets `^^`
  name a structured binding, and whether parameter reflections compare equal across
  redeclarations ([expr.reflect]). The representation holds either way.
- **The presumed file name in `pp`'s value** (§8.7): confirm with the preprocessor work how the
  spelled include path reaches the child `pp` (key data or a recorded question).
- **`.debug_macro`** (DWARF 5): a client option with no effect on the AST.
- **Keeping open documents' trees in memory** under eviction (§9.6): part of Part E's eviction
  constants.

Follow-up implementation issues, opened when this design is accepted:

| Issue | Area | Content |
| --- | --- | --- |
| AST core | `area:ast` | The handle types, `handles<…>`, the interners with per-kind pools, reflection-derived fingerprint, equality, walking and compaction, the red view |
| Types and constants | `area:ast` | §5: type kinds, `canon[]`, construction-time canonicalization, `values` with the `VDep*` family, template arguments |
| Locations | `area:pp`, `area:ast` | §8: the region's location table, `tu_item_index`, `line_table`, `render`, LSP mapping |
| Engine: `prefix_after_item` | `area:query-engine` | §4.2, as a comment on #16 rather than a new issue |
