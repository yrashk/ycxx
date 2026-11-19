# Closure types (θ, #280)

How ycxx represents a lambda-expression's closure type ([expr.prim.lambda.closure]) and its
function call operator. It amends ast.md §4.3–4.4 for lambdas and is summarized in DECISIONS
§4.9. The decisions came from Fable (#280; DECISIONS §6.5); the session checked them against
Clang 23 and GCC 16 (the probes at the end) and wrote this document.

## Decisions

1. **The closure type is a class entity.** `EntityKind::class_` with no name; `scope` is the
   enclosing entity (decision 3); `head` mixes the lambda's signature (the canonical parameter
   types of `operator()`, and in step 3 its template parameters' kinds) and its ordinal.
   *Reason:* every class path (`class_type`, `class_rec`, layout, special members) keys on
   `class_`. *Rejected:* a new `closure` kind (kind checks everywhere for nothing new); a
   generated name such as `__lambda0` (the name is not ABI data, the signature and ordinal are).
2. **Ordinal = the earlier closures of the same enclosing entity and signature, in the order of
   their closing `}`** (Itanium C++ ABI §5.1.8 numbers per signature: `UlvE_`, `UlvE0_`, …;
   §5.1.9 makes the `}` the lambda's key). A closure in another lambda's introducer completes
   first, so every closure's ordinal is known at its own `{`. *Rejected:* numbering by `[`
   (differs on `[x = []{}]{}`); one counter for all signatures.
3. **Enclosing entity:** a function body's lambda belongs to the function, or to the
   specialization being instantiated; a default member initializer's to the data member, a
   variable's or static data member's initializer's to the variable. Default arguments and
   namespace-scope expressions with no declaration (a `static_assert`) give a TU-local closure.
   *Rejected:* the enclosing item (an item is not an ABI context). Step 2 implements lambdas in
   function bodies; the other contexts are #338.
4. **Linkage:** a closure's members have the vague linkage of an inline member function when its
   enclosing entity has external linkage and a definition that may appear in several units
   (inline functions and variables, templates' specializations, static and default member
   initializers of classes with linkage). Otherwise they are internal symbols, with the same
   Itanium spelling (Clang spells those `$_N`; any unique local name links). A function
   template specialization for a TU-local closure type is TU-local too ([basic.link]/4).
5. **Mangling:** a closure in a function is `<local-name>`: `Z <encoding> E` then
   `Ul <signature> E [<n>] _`, `v` for no parameters; in a variable or data member it is
   `<closure-prefix>`, `<name> M`. Members: `cl` (`K` unless `mutable`), `cv <type>` for the
   conversion to a function pointer, `8__invoke` for the static invoker.
6. **The body is analysed inside the enclosing analysis** (`analyze_body` or `item_sema`),
   which records one `ClosureDef` per lambda in completion order: the closure and its members'
   entities, the body tree and its own locals, the conversion and invoker bodies, the lambda's
   token extent, and its flags (mutable, captureless, `this` or `*this`, default capture).
   *Reason:* implicit captures and name lookup depend on the enclosing scopes mid-parse.
   *Rejected:* a cell per lambda keyed by `(item, offset)` (it would rebuild those scopes and parse
   the body twice; step 3 adds such a re-analysis for generic bodies only).
7. **Names in the body resolve through a closure frame** in `Sema`: a local of an enclosing
   function is already captured (a `Member` of `This` on its field), captured now by the default
   (outermost frame first in nested lambdas, [expr.prim.lambda.capture]/7), not odr-used (a
   constant's value, no capture), or an error. `this` in the body is the captured pointer, or the
   address of the `*this` copy; lookup of members still uses the enclosing class.
   *Reason:* irgen and the interpreter already read `Member` of `This`; a field's index is fixed
   at its first capture, so no node needs patching. *Rejected:* a `CaptureRef` node (a second
   path for what `Member` does); resolving captures after the body (trees are hash-consed).
8. **Fields in capture order:** explicit captures in the capture list's order, then implicit
   captures in order of first use. By copy: the captured object's type (a reference's referee);
   by reference: a reference member; `this`: a pointer; `*this`: the class; an init-capture: its
   deduced type (step 1's deduction). The `ClassRec` is built privately and published at `}`,
   complete, before the lambda-expression's node exists (#156: published once).
9. **The lambda-expression is a `Lambda` node**: the closure type, a prvalue, and one
   `MemberInit{field, type, init}` per field in field order, so a reference member is
   initialized with an address. irgen and the interpreter lower it as an aggregate's `InitList`.
   *Rejected:* `InitList` itself (the closure entity belongs on the node); a constructor (closures
   have none to call).
10. **Emission:** the front end turns each `ClosureDef` of an emitted body into `FunctionDef`s:
    `operator()` (a member, `this_type` from the frame), and for a captureless closure the
    static invoker (`return Closure{}(params...)`, [expr.prim.lambda.closure]/9). The conversion
    to a pointer to function is the invoker's address, made by sema; the conversion function's
    own symbol is not emitted yet (#339). Implicit copy, move and destructor come from
    `special_members` when the record is published.
11. **Templates:** a lambda in a template's definition is analysed with dependent types and
    emits nothing; an instantiation re-analyses the body, so each specialization has its own
    closure. Step 3 instantiates a generic `operator()` by re-analysing the recorded token extent
    with its template parameters bound and the frame rebuilt from the record; the closure's
    fields are fixed by the definition ([expr.prim.lambda.capture]/7). Step 3 does this in a
    `BodyAnalysis` over the enclosing item with the enclosing function's context, its constants
    as locals, and `Sema::open_generic`; the definition registers itself with `Program` at its
    `}` so a part being analysed can be instantiated from elsewhere.
12. **Constant evaluation (step 3, #281):** closures are class objects; `function_ir` of an
    `operator()` is found through a map from entity to `ClosureDef`, filled at each `}`, so a
    lambda called in a constant expression of the same item works.
13. **Incrementality:** an earlier lambda of the same signature shifts later closures' ids, as
    ast.md §4.5 says of local ids; the mangled names change with it, by the ABI.

## Probes (Clang 23, GCC 16; 2026-11-19)

- Numbering per signature: in `inline void f()`, `[]{}`, `[](int){}`, `[]{}` are `UlvE_`,
  `UliE_`, `UlvE0_`; in `[x = []{ return 7; }]{ return x() + 1; }` the inner closure is numbered
  before the outer (called by name: `UlvE0_` returns 7, `UlvE1_` 8).
- Contexts: `inline auto v = []{…}` → `_ZNK1vMUlvE_clEv`; `static inline auto m` in `S` →
  `_ZNK1S1mMUlvE_clEv`; a default member initializer of `S::d` → `_ZNK1S1dMUlvE_clEv`; a variable
  template `vt<int>` → `_ZNK2vtIiEMUlvE_clEv`; default arguments of an inline function, and
  closures in non-inline functions and variables, are local `$_N` symbols.
- Invoker and conversion: `_ZZ1hvENUlvE_8__invokeEv`, `_ZZ1hvENKUlvE_cvPFivEEv`.
- Generic: `[](auto){}` is `UlT_E_` with `clIiEEDaS_`; `[]<class T>(T, int){}` is `UlTyT_iE_` with
  `clIiEEDaS_i`.
- Layout, same in both: `[d, c, i]` of `char, char, int` is 8 bytes `d c _ _ i`; `[=]` using
  `i, c, d` lays out `i c d`; reference captures are 8 bytes each; `[this]` is a pointer;
  `[*this]` the object.
