# Local classes (#117)

How ycxx represents a class defined in a block ([class.local]). It amends ast.md §4.4 for
local class definitions and is summarized in DECISIONS §4.9. Fable made the decisions (#117,
DECISIONS §6.5); the session checked them against Clang 23 and GCC 16 and wrote this document.

## Decisions

1. **A local class's definition is an item; its block is a recorded value.** The splitter cuts the
   class-specifier as a `class_def` item with member items, and `ItemAnalysis`, `class_def` and
   `BodyAnalysis` analyse it as they do any class. *Reason:* the class machinery (member
   declarations, special members and their triviality, default member initializers, emission,
   `function_ir`, instantiation) is keyed by item. *Rejected:* analysing the class inside the
   body like a closure (closures.md decision 6), which would be a second class path;
   sharing the body's `Sema`, which is gone when member bodies are analysed lazily.
2. **ast.md §4.4 is amended for local class definitions only.** §4.4 objects to ids that depend on
   the body. A local item's id is its file, scope path, its owner's id, own tokens and ordinal
   (items.cppm), so it survives edits elsewhere in the body, as §4.5 says of any item, and two
   same-shaped local classes of different functions are different items. Block variables, bindings,
   lambdas and elaborated declarations in blocks stay data of the body.
3. **The local item's shape.** `Item::local_owner` is the item whose function body (or
   declaration, for a lambda in an initializer) holds it, which is also its parent; the owner
   lists it among its members, so items stay in preorder. The item is the class-specifier only,
   from its class-key to its `}`: the declarators after it are the block's. Its scope path has a
   `(local)` segment no source name spells, and the name index gives it a namespace level of its
   own, named by its id, so no lookup from outside its block finds its names.
4. **The splitter's test.** In a function body, a consteval block, or a declaration's tokens, a
   class-key (not after `enum`, `->` or `new`) followed by attributes, an unqualified name or
   none, `final`, then `{` or a base-clause and `{`. An anonymous union or struct (`union { … };`)
   is not a local class ([class.union.anon]).
5. **The block defines the class when its analysis reaches it.** The body `Parser` calls the
   `define_class` hook at the class-key; `Program::define_local_class` makes the entity, records
   the block, declares the name in the block, analyses the item and completes the class, and the
   parser continues after the `}`. Any other path to a local item (the front end's item loop,
   `function_ir`) first analyses the owner, once (`Program::local_class`); an owner whose
   analysis never reaches the class (an error first) leaves its items empty and silent.
6. **Identity.** `EntityKind::class_` with the class's name (none if unnamed), its `scope` the
   function it is local to (closures.md decision 3: the innermost lambda's `operator()`, or the
   specialization being instantiated), `local_class` set, and `head` mixing its ordinal: the
   earlier local classes of that function with its name, in token order. Unnamed classes count
   among the function's unnamed ones; `typedef struct { … } T;` names the class `T`
   ([dcl.typedef]/9). *Rejected:* closures' completion order (Itanium numbers lambdas that way,
   not local classes). Where Clang and GCC number differently (a forward declaration, a
   typedef-named class), ycxx follows GCC for now (#1448).
7. **Linkage** is a closure's (closures.md decision 4): external when the function's definition
   may appear in several units (inline, defined in its class, a specialization), else internal.
8. **Mangling** is the Itanium `<local-name>`: `Z <encoding> E <name> [<discriminator>]`, with
   `_ <n>` (or `__ <n> _`) for the n + 2-th class of one name and `Ut [<n>] _` for unnamed ones;
   a member's discriminator follows its whole nested-name (`_ZZ1hvENK1A3getE_0v`). Checked
   against Clang 23 objects (`tests/own/abi/local-class-names.run.cpp`).
9. **The block, as recorded** (`sema::OuterBlock`, taken at the class-head): its names innermost
   first and one per name (locals with constants' values, block types, using-declarations,
   namespace aliases, which qualified names use too), its `using enum`s and using-directives (which
   namespace lookup from the members uses), the template-heads and arguments, the
   function and its class, and the enclosing local class's own block. Names declared later in the
   block are invisible to the class, as [basic.scope.pdecl] says.
10. **Lookup from a member** goes: its blocks, the local class (and classes nested in it), the
    recorded block, the function's class and the classes around it, then namespaces
    ([basic.lookup.unqual]). An automatic variable found in the block is usable only unevaluated
    or as a constant's value ([basic.def.odr]/10); a static one names its variable. A replayed part
    (a default member initializer, a default argument) asks its questions for the block
    (`Questions::local_outer_block`). Access is the function's ([class.local]/1).
11. **Completeness.** Outside its definition a local class is complete once its record is
    published. A point inside a local class counts as its owner's, in a body (a complete-class
    context of the classes around a member function, [class.mem.general]/7); declarations and
    definitions in local items count as at their owner (`Program::declared_at`).
12. **Templates.** In a template's definition the local class is a templated entity: its type is
    dependent (`EntityRec::closure_pattern`) and nothing is emitted for it. A specialization of the
    function gets its own class, scoped to the specialization: its members are the definition's
    items analysed again with the arguments bound (`replay_class`), and their bodies instantiate
    as class template members do ([temp.inst]/3). The block's template-heads are the local items'
    `enclosing_heads`.
13. **A block's elaborated-type-specifier** naming no class (`struct X* p;`), or `struct X;` unless
    the block already declares X, declares an incomplete local class in the block
    ([basic.scope.pdecl]/7, [dcl.type.elab]); a definition later in the same block completes that
    entity ([class.name]).
14. **Restrictions** diagnosed on a local class's members: member templates ([temp.mem]/2), static
    data members ([class.static.data]) and friend function definitions ([class.friend]/6).

## Not yet

Block-scope enumerations (#1442); a member class of a local class defined later in the block
(#1443); local classes with virtual functions (#1155); which uses of an enclosing constant are
odr-uses, shared with lambdas (#1447). Constant evaluation of objects with class
or array subobjects (#1140) limits local aggregates of those shapes in constant expressions too.
