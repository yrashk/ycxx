// ycxx.items: the item splitter (#18; query-engine.md §B1.2, ast.md §3.1, §4.1). A token-level
// pre-pass cuts a region's tokens into items before parsing: depth-0 `;`, a function body's `}`,
// `template<…>` heads attached to what follows, and nested item lists for namespaces, linkage
// specifications, export blocks and class bodies. Each item gets a skeleton (its kind, declared
// names, spelled qualifier, template depth, body) and an id that excludes bodies and positions.
//
// The parser confirms the boundaries; a disagreement is a splitter bug, never a soundness issue,
// because the parser works from the item sequence (§B1.2).
//
// Seams:
// - Ids are 128-bit fingerprints. The `items` and `tokens` interners (ast.md §9, placeholders in
//   ycxx.ast) turn them into `ItemId`/`BodyId` handles; `Item` holds what they need.
// - The occurrence salt of §B1.2 is a function of the prefix, so it is added where prefixes exist
//   (#16), not here.
// - Child regions inside an item (`enum E { #include "v.h" };`) are flattened by `region_items`
//   once `pp` has includes (M1).
export module ycxx.items;

import std;
import ycxx.engine;
export import ycxx.pp;

export namespace ycxx::items {

using lex::Tk;
using lex::Tok;
using lex::u32;
using lex::u8;

enum class ItemKind : u8 {
  empty,           // `;`
  declaration,     // a declaration ending in `;`: variables, function declarations, aliases, …
  function,        // a function definition: declaration part and body
  class_def,       // a class-specifier, with its members as nested items, and its declarators
  enum_def,        // an enum-specifier, and its declarators
  namespace_def,   // `namespace N { … }`, members nested
  linkage_block,   // `extern "C" { … }`, members nested
  export_block,    // `export { … }`, members nested
  access_spec,     // `public:` in a class
  consteval_block, // `consteval { … }` ([dcl.pre]); the compound statement is its body
  stray,           // tokens that start no item (an unmatched `}`)
};

// What a declared name is, as far as tokens tell (ast.md §3.1).
namespace name_flag {
inline constexpr u8 type = 1;        // a class, enum, typedef or alias name
inline constexpr u8 template_ = 2;   // declared by a template
inline constexpr u8 namespace_ = 4;  // a namespace name
inline constexpr u8 unknown = 8;     // a using-declaration's name: unknown until lookup
inline constexpr u8 definition = 16; // the declaration is a definition
inline constexpr u8 friend_ = 32;    // a friend declaration: indexed apart ([class.friend])
inline constexpr u8 enumerator = 64; // an unscoped enumerator, declared into the enclosing scope
inline constexpr u8 specialization = 128;  // an explicit specialization's: no name lookup finds ([temp.expl.spec])
}  // namespace name_flag

// One name an item declares, in token order; its ordinal is its `DeclOrd` number (ast.md §4.1).
struct DeclName {
  std::string name;  // empty for an unnamed class head, otherwise the spelling used for lookup
  u32 offset = 0;    // of its token, relative to the item's first token
  u32 ordinal = 0;
  u8 flags = 0;
  bool operator==(const DeclName&) const = default;
};

inline constexpr u32 none = 0xffffffffu;

struct Item {
  ItemKind kind = ItemKind::declaration;
  u32 begin = 0;  // the item's tokens in the region: [begin, end)
  u32 end = 0;
  // A function's body ([dcl.fct.def.general] function-body: a ctor-initializer and compound
  // statement, a function-try-block, `= default;` or `= delete…;`), or a consteval block's
  // compound statement: [body_begin, body_end). Empty when body_begin == body_end.
  u32 body_begin = 0;
  u32 body_end = 0;
  // The braces of a class, enum, namespace, linkage or export block: [brace_begin, brace_end),
  // from `{` to after `}`. Empty when equal.
  u32 brace_begin = 0;
  u32 brace_end = 0;
  u32 template_depth = 0;  // the number of template-heads
  bool specialization = false;  // `template<>`: an explicit specialization ([temp.expl.spec])
  // The spelled nested-name-specifier of the first declarator (`N::S::` in `void N::S::f()`), as
  // region tokens [qualifier_begin, qualifier_end).
  u32 qualifier_begin = 0;
  u32 qualifier_end = 0;
  std::vector<DeclName> names;
  std::vector<u32> members;  // nested items, by index
  u32 parent = none;
  std::string scope;  // the syntactic scope path: enclosing namespaces and classes, `::`-joined
  u32 unnamed_type_ordinal = none;  // the class scope's unnamed types in declaration order (Itanium §5.1.5.5)
  u32 ordinal = 0;    // earlier siblings with the same scope and declaration-part tokens
  Fp128 id;           // §B1.2: file identity, scope path, declaration-part tokens, ordinal
  Fp128 body;         // the body's tokens; zero without a body
  bool operator==(const Item&) const = default;

  bool has_body() const { return body_end > body_begin; }
  // The number of tokens in the declaration part (ast.md §3.1): everything but the body.
  u32 decl_length() const { return end - begin - (body_end - body_begin); }
};

// The item tree of a region, preorder: an item's members follow it.
struct Items {
  std::vector<Item> items;
  std::vector<u32> top;  // the region's own item list
  bool operator==(const Items&) const = default;
};

// The fingerprint of a token for identity: its kind and content, not its whitespace flags or
// alternative spelling.
void hash_token(Hasher& h, const pp::Region& r, Tok t);

// Splits a region. `file` is the file's identity in item ids.
Items split(const pp::Region& r, Fp128 file);

// `region_items` (§B1.3), keyed by file until regions are content-addressed handles.
struct RegionItems {
  using Key = pp::FileId;
  using Value = Items;
  static engine::query_task<Value> run(engine::Ctx& cx, Key file);
};

}  // namespace ycxx::items

namespace ycxx::items {

// `__remove_cv` and the other type-transforming built-ins at token `j`: a type specifier with its
// operand in parentheses (docs/EXTENSIONS.md), not a declarator-id.
bool is_transform_trait(const pp::Region& r, u32 j) {
  if (j >= r.tokens.size() || r.tokens[j].kind != Tk::identifier) return false;
  const lex::TypeTraitInfo* t = lex::type_trait(r.name(r.tokens[j]));
  return t && t->transforms;
}

using namespace lex;

bool is_class_key(Tk k) { return k == Tk::kw_class || k == Tk::kw_struct || k == Tk::kw_union; }

class Splitter {
public:
  Splitter(const pp::Region& r, Items& out) : r_(r), out_(out) {}

  // Splits items until `}` (not consumed) or the end, into `list`.
  void list(u32 parent, const std::string& scope, std::vector<u32>& list) {
    u32 unnamed_types = 0;
    while (kind(i_) != Tk::eof && kind(i_) != Tk::r_brace) list.push_back(item(parent, scope, unnamed_types));
  }
  // At the top level an unmatched `}` is a stray item.
  void top() {
    for (;;) {
      list(none, "", out_.top);
      if (kind(i_) == Tk::eof) break;
      u32 k = add(none, "");
      at(k).kind = ItemKind::stray;
      at(k).begin = i_;
      at(k).end = ++i_;
      out_.top.push_back(k);
    }
  }

private:
  Tk kind(u32 j) const { return j < r_.tokens.size() ? r_.tokens[j].kind : Tk::eof; }
  bool is_name(u32 j, std::string_view s) const { return kind(j) == Tk::identifier && r_.name(r_.tokens[j]) == s; }
  bool transform_trait(u32 j) const { return is_transform_trait(r_, j); }
  Item& at(u32 k) { return out_.items[k]; }
  u32 add(u32 parent, const std::string& scope) {
    Item it;
    it.parent = parent;
    it.scope = scope;
    out_.items.push_back(std::move(it));
    return static_cast<u32>(out_.items.size() - 1);
  }

  // The index after the bracket that closes the one at `j`; at most the eof.
  u32 match(u32 j) const {
    Tk open = kind(j);
    Tk close = open == Tk::l_paren ? Tk::r_paren : open == Tk::l_square ? Tk::r_square : Tk::r_brace;
    std::vector<Tk> stack{close};
    for (++j; kind(j) != Tk::eof; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren) stack.push_back(Tk::r_paren);
      else if (k == Tk::l_square) stack.push_back(Tk::r_square);
      else if (k == Tk::l_brace) stack.push_back(Tk::r_brace);
      else if (k == Tk::r_paren || k == Tk::r_square || k == Tk::r_brace) {
        // A mismatched closer ends every group up to its own.
        while (!stack.empty() && stack.back() != k) stack.pop_back();
        if (!stack.empty()) stack.pop_back();
        if (stack.empty()) return j + 1;
      }
    }
    return j;
  }
  // After a template-head's `<…>`: angle brackets balanced outside parentheses ([temp.param]
  // requires `>` in a default argument to be parenthesized).
  u32 match_angle(u32 j) const {
    int depth = 0;
    for (; kind(j) != Tk::eof; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren || k == Tk::l_square || k == Tk::l_brace) {
        j = match(j) - 1;
      } else if (k == Tk::less) {
        ++depth;
      } else if (k == Tk::greater || k == Tk::greatergreater) {
        depth -= k == Tk::greater ? 1 : 2;
        if (depth <= 0) return j + 1;
      } else if (k == Tk::semi || k == Tk::r_brace) {
        return j;
      }
    }
    return j;
  }

  u32 item(u32 parent, const std::string& scope, u32& unnamed_types);
  void general(u32 k, u32& unnamed_types);
  u32 ctor_initializer(u32 j);
  // Whether the tokens between a class-key and `{` are a class-head ([class.pre]): attributes,
  // one (qualified, possibly template) name, `final`; not `struct S s{1}`.
  bool is_class_head(u32 key, u32 open) const {
    bool named = false;
    for (u32 n = key + 1; n < open; ++n) {
      Tk t = kind(n);
      if (t == Tk::l_square || ((t == Tk::kw_alignas || is_name(n, "__attribute__")) && kind(n + 1) == Tk::l_paren)) {
        n = match(t == Tk::l_square ? n : n + 1) - 1;
      } else if (t == Tk::kw_class && kind(key) == Tk::kw_enum && n == key + 1) {
        // `enum class`
      } else if (t == Tk::kw_struct && kind(key) == Tk::kw_enum && n == key + 1) {
      } else if (t == Tk::identifier && !(named && is_name(n, "final"))) {
        if (named && kind(n - 1) != Tk::coloncolon) return false;
        named = true;
      } else if (t == Tk::less) {
        n = angle_end(n) - 1;
      } else if (t != Tk::coloncolon) {
        return false;
      }
    }
    return true;
  }
  // After template arguments starting at the `<` at `j`.
  u32 angle_end(u32 j) const { return match_angle(j); }
  // Splits the member list in the braces at `open` into `k`'s members; returns the index after `}`.
  u32 nested(u32 k, u32 open, const std::string& scope);
  // The name a class-head declares: the last identifier outside template arguments, attributes
  // and `final` ([class.pre]); empty for an unnamed class.
  u32 class_name_token(u32 key, u32 end) const {
    u32 at = 0;
    int angle = 0;
    for (u32 n = key + 1; n < end; ++n) {
      Tk t = kind(n);
      if (t == Tk::l_square || t == Tk::l_paren) n = match(n) - 1;
      else if (t == Tk::less) ++angle;
      else if (t == Tk::greater) --angle;
      else if (t == Tk::greatergreater) angle -= 2;
      else if (t == Tk::colon && angle <= 0) break;
      else if (t == Tk::identifier && angle <= 0 && !is_name(n, "final")) at = n;
    }
    return at;
  }
  // The scope a class's members are in: its name, and the template arguments of a specialization
  // (`X<T*>`), so a partial or explicit specialization's members are not its template's.
  std::string class_scope(u32 name, u32 end) const {
    std::string r(r_.name(r_.tokens[name]));
    if (kind(name + 1) == Tk::less)
      for (u32 n = name + 1, close = match_angle(name + 1); n < close && n < end; ++n) r += r_.spelling(r_.tokens[n]);
    return r;
  }

  const pp::Region& r_;
  Items& out_;
  u32 i_ = 0;

};

u32 Splitter::item(u32 parent, const std::string& scope, u32& unnamed_types) {
  u32 k = add(parent, scope);
  u32 j = i_;
  at(k).begin = j;
  if (kind(j) == Tk::semi) {
    at(k).kind = ItemKind::empty;
    at(k).end = i_ = j + 1;
    return k;
  }
  // Attributes and `export` before the declaration.
  for (;;) {
    if (kind(j) == Tk::l_square && kind(j + 1) == Tk::l_square) j = match(j);
    else if (kind(j) == Tk::kw_alignas && kind(j + 1) == Tk::l_paren) j = match(j + 1);
    else break;
  }
  if (kind(j) == Tk::kw_export) {
    if (kind(j + 1) == Tk::l_brace) {
      at(k).kind = ItemKind::export_block;
      at(k).end = nested(k, j + 1, scope);
      return k;
    }
    ++j;
  }
  if (kind(j) == Tk::kw_extern && kind(j + 1) == Tk::string_lit) {
    u32 s = j + 1;
    while (kind(s + 1) == Tk::string_lit && r_.tokens[s + 1].has(tok_flag::continued)) ++s;
    if (kind(s + 1) == Tk::l_brace) {
      at(k).kind = ItemKind::linkage_block;
      at(k).end = nested(k, s + 1, scope);
      return k;
    }
  }
  u32 ns = kind(j) == Tk::kw_inline ? j + 1 : j;
  if (kind(ns) == Tk::kw_namespace) {
    // `namespace N = M;` is an alias: a declaration.
    u32 h = ns + 1;
    while (kind(h) == Tk::identifier || kind(h) == Tk::coloncolon || kind(h) == Tk::kw_inline ||
           (kind(h) == Tk::l_square && kind(h + 1) == Tk::l_square) || (kind(h) == Tk::l_paren && kind(h - 1) == Tk::identifier)) {
      h = kind(h) == Tk::l_square || kind(h) == Tk::l_paren ? match(h) : h + 1;  // `__attribute__((…))` too
    }
    if (kind(h) == Tk::l_brace) {
      at(k).kind = ItemKind::namespace_def;
      std::string inner = scope;
      for (u32 n = ns + 1; n < h; ++n) {
        if (kind(n) == Tk::l_square || (kind(n) == Tk::identifier && kind(n + 1) == Tk::l_paren)) {
          n = match(kind(n) == Tk::l_square ? n : n + 1) - 1;  // past attributes
          continue;
        }
        if (kind(n) != Tk::identifier) continue;
        if (!inner.empty()) inner += "::";
        inner += r_.name(r_.tokens[n]);
      }
      if (h == ns + 1) inner += scope.empty() ? "(anonymous)" : "::(anonymous)";
      at(k).end = nested(k, h, inner);
      return k;
    }
  }
  if (kind(j) == Tk::kw_consteval && kind(j + 1) == Tk::l_brace) {
    at(k).kind = ItemKind::consteval_block;
    at(k).body_begin = j + 1;
    at(k).body_end = at(k).end = i_ = match(j + 1);
    return k;
  }
  if ((kind(j) == Tk::kw_public || kind(j) == Tk::kw_protected || kind(j) == Tk::kw_private) &&
      kind(j + 1) == Tk::colon) {
    at(k).kind = ItemKind::access_spec;
    at(k).end = i_ = j + 2;
    return k;
  }
  // Template heads; `template<>` begins an explicit specialization, which is no template
  // ([temp.expl.spec]/1).
  while (kind(j) == Tk::kw_template && kind(j + 1) == Tk::less) {
    if (kind(j + 2) != Tk::greater) ++at(k).template_depth;
    else at(k).specialization = true;
    j = match_angle(j + 1);
  }
  i_ = j;
  general(k, unnamed_types);
  return k;
}

u32 Splitter::nested(u32 k, u32 open, const std::string& scope) {
  i_ = open + 1;
  std::vector<u32> members;
  list(k, scope, members);
  at(k).members = std::move(members);
  if (kind(i_) == Tk::r_brace) ++i_;
  at(k).brace_begin = open;
  at(k).brace_end = i_;
  return i_;
}

// A ctor-initializer from the `:` at `j` to the end of the compound statement: the body's `{` is
// the first one that ends a mem-initializer list (after `)`, `}` or `...`).
u32 Splitter::ctor_initializer(u32 j) {
  for (++j; kind(j) != Tk::eof && kind(j) != Tk::semi; ++j) {
    Tk k = kind(j);
    if (k == Tk::l_paren || k == Tk::l_square) {
      j = match(j) - 1;
    } else if (k == Tk::l_brace) {
      Tk prev = kind(j - 1);
      if (prev == Tk::r_paren || prev == Tk::r_brace || prev == Tk::ellipsis) return j;
      j = match(j) - 1;
    }
  }
  return j;
}

// Whether a token can begin a declaration but not continue the declarators after a class or
// enum body (`struct S { … } const s;` continues; `struct S { … } int main()` does not).
bool begins_declaration_only(Tk t) {
  switch (t) {
  case Tk::kw_void: case Tk::kw_bool: case Tk::kw_char: case Tk::kw_char8_t: case Tk::kw_char16_t:
  case Tk::kw_char32_t: case Tk::kw_wchar_t: case Tk::kw_short: case Tk::kw_int: case Tk::kw_long:
  case Tk::kw_signed: case Tk::kw_unsigned: case Tk::kw_float: case Tk::kw_double: case Tk::kw_auto:
  case Tk::kw_struct: case Tk::kw_class: case Tk::kw_union: case Tk::kw_enum: case Tk::kw_namespace:
  case Tk::kw_template: case Tk::kw_using: case Tk::kw_static_assert: case Tk::kw_export:
    return true;
  default: return false;
  }
}

// Any other item: a declaration, or a definition of a function, class or enum.
void Splitter::general(u32 k, u32& unnamed_types) {
  u32 j = i_;
  bool initializer = false;  // a depth-0 `=` was seen: braces are initializers
  bool params = false;       // a declarator's parameter list was seen
  u32 class_key = none;      // the class-key or `enum` of a class-head, if any
  bool base_clause = false;  // a `:` after the class-key, before any parameter list
  bool typedef_ = false;
  auto finish_body = [&](u32 body_begin, u32 end) {
    at(k).kind = ItemKind::function;
    at(k).body_begin = body_begin;
    at(k).body_end = at(k).end = i_ = end;
  };
  for (;;) {
    Tk t = kind(j);
    Tk prev = j > at(k).begin ? kind(j - 1) : Tk::eof;
    switch (t) {
    case Tk::eof:
    case Tk::r_brace:  // the enclosing list ends: the item is cut here
      at(k).end = i_ = j;
      return;
    case Tk::semi:
      at(k).end = i_ = j + 1;
      return;
    case Tk::l_paren:
      if (!initializer && prev != Tk::kw_decltype && prev != Tk::kw_noexcept && prev != Tk::kw_alignas &&
          prev != Tk::kw_sizeof && prev != Tk::kw_alignof && prev != Tk::kw_explicit && prev != Tk::kw_typeid &&
          prev != Tk::kw_static_assert && prev != Tk::kw_asm && prev != Tk::kw_requires &&
          !(prev == Tk::kw_operator) &&
          // a GNU attribute or asm label, or a type-transforming built-in (docs/EXTENSIONS.md)
          !(j > at(k).begin && (is_name(j - 1, "__attribute__") || is_name(j - 1, "__asm") || is_name(j - 1, "__asm__") ||
                                transform_trait(j - 1)))) {
        params = true;
      }
      j = match(j);
      continue;
    case Tk::l_square:
      j = match(j);
      continue;
    case Tk::kw_operator:
      // `operator=`, `operator()`: the operator's tokens are no `=` and no parameter list.
      if (kind(j + 1) == Tk::l_paren && kind(j + 2) == Tk::r_paren) j += 3;
      else if (kind(j + 1) == Tk::l_square && kind(j + 2) == Tk::r_square) j += 3;
      else j += 2;
      continue;
    case Tk::equal:
      if (params && !initializer && (kind(j + 1) == Tk::kw_default || kind(j + 1) == Tk::kw_delete)) {
        u32 e = j + 1;
        while (kind(e) != Tk::semi && kind(e) != Tk::eof && kind(e) != Tk::r_brace) {
          e = kind(e) == Tk::l_paren ? match(e) : e + 1;
        }
        finish_body(j, kind(e) == Tk::semi ? e + 1 : e);
        return;
      }
      initializer = true;
      ++j;
      continue;
    case Tk::colon:
      if (class_key != none && !params) {
        base_clause = true;
      } else if (params && !initializer && class_key == none) {
        u32 open = ctor_initializer(j);
        finish_body(j, kind(open) == Tk::l_brace ? match(open) : open);
        return;
      }
      ++j;
      continue;
    case Tk::kw_try:
      if (params && !initializer) {
        u32 body = j;
        u32 open = kind(j + 1) == Tk::colon ? ctor_initializer(j + 1) : j + 1;
        u32 e = kind(open) == Tk::l_brace ? match(open) : open;
        while (kind(e) == Tk::kw_catch && kind(e + 1) == Tk::l_paren) {
          u32 handler = match(e + 1);
          e = kind(handler) == Tk::l_brace ? match(handler) : handler;
        }
        finish_body(body, e);
        return;
      }
      ++j;
      continue;
    case Tk::kw_requires:
      // A requires-expression, after an operator or another `requires`: its parameter list and
      // its requirement braces are no declarator and no body.
      if (prev == Tk::kw_requires || prev == Tk::ampamp || prev == Tk::pipepipe || prev == Tk::exclaim ||
          prev == Tk::l_paren || prev == Tk::equal || prev == Tk::comma) {
        ++j;
        if (kind(j) == Tk::l_paren) j = match(j);
        if (kind(j) == Tk::l_brace) j = match(j);
        continue;
      }
      ++j;
      continue;
    case Tk::kw_class:
    case Tk::kw_struct:
    case Tk::kw_union:
    case Tk::kw_enum:
      if (class_key == none && !initializer && !params) class_key = j;
      ++j;
      continue;
    case Tk::kw_typedef:
      typedef_ = true;
      ++j;
      continue;
    case Tk::l_brace:
      if (initializer) {
        j = match(j);
        continue;
      }
      if (class_key != none && (base_clause || (!params && is_class_head(class_key, j)))) {
        // A class or enum body; declarators may follow it.
        bool is_enum = kind(class_key) == Tk::kw_enum;
        u32 name = class_name_token(class_key, j);
        u32 alias = name == 0 && typedef_ ? match(j) : 0;
        bool named_by_typedef = alias != 0 && kind(alias) == Tk::identifier;
        u32 parent = at(k).parent;
        if (name == 0 && !named_by_typedef && parent != none && at(parent).kind == ItemKind::class_def)
          at(k).unnamed_type_ordinal = unnamed_types++;
        at(k).kind = is_enum ? ItemKind::enum_def : ItemKind::class_def;
        if (is_enum) {
          at(k).brace_begin = j;
          j = at(k).brace_end = match(j);
        } else {
          std::string inner = at(k).scope;
          if (!inner.empty()) inner += "::";
          if (name != 0) inner += class_scope(name, j);
          else if (named_by_typedef) inner += r_.name(r_.tokens[alias]);
          else if (at(k).unnamed_type_ordinal != none) inner += std::format("(unnamed type {})", at(k).unnamed_type_ordinal);
          else inner += "(anonymous)";
          j = nested(k, j, inner);
        }
        // A `;` missing after the body ([class.pre], [dcl.enum]): what follows can only begin a
        // new declaration, so the item ends at the brace and its analysis reports the `;`.
        if (begins_declaration_only(kind(j))) {
          at(k).end = i_ = j;
          return;
        }
        class_key = none;
        base_clause = false;
        params = false;
        continue;
      }
      if (params) {
        finish_body(j, match(j));
        return;
      }
      j = match(j);  // a brace initializer, `int x{1}`
      continue;
    default:
      ++j;
      continue;
    }
  }
}

}  // namespace ycxx::items

namespace ycxx::items {

// The skeleton's declared names (ast.md §3.1), from the tokens of an item whose boundaries are
// known. Lookup-free: at namespace and class scope every item is a declaration, so its
// declarator-ids can be found from tokens, constructors included ([class.ctor.general]).
class Names {
public:
  Names(const pp::Region& r, Items& out) : r_(r), out_(out) {}

  void run(u32 k) {
    Item& it = out_.items[k];
    switch (it.kind) {
    case ItemKind::namespace_def: namespace_names(k); break;
    case ItemKind::class_def:
    case ItemKind::enum_def: class_names(k); break;
    case ItemKind::declaration:
    case ItemKind::function: declaration_names(k, start_of(k), it.has_body() ? it.body_begin : it.end); break;
    default: break;
    }
    for (u32 m : out_.items[k].members) run(m);
  }

private:
  Tk kind(u32 j) const { return j < r_.tokens.size() ? r_.tokens[j].kind : Tk::eof; }
  std::string_view name(u32 j) const { return r_.name(r_.tokens[j]); }
  bool is_name(u32 j, std::string_view s) const { return kind(j) == Tk::identifier && name(j) == s; }
  bool transform_trait(u32 j) const { return is_transform_trait(r_, j); }
  u32 match(u32 j) const {
    int depth = 0;
    for (; kind(j) != Tk::eof; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren || k == Tk::l_square || k == Tk::l_brace) ++depth;
      else if (k == Tk::r_paren || k == Tk::r_square || k == Tk::r_brace) {
        if (--depth <= 0) return j + 1;
      }
    }
    return j;
  }
  u32 match_angle(u32 j, u32 limit) const {
    int depth = 0;
    for (; j < limit; ++j) {
      Tk k = kind(j);
      if (k == Tk::l_paren || k == Tk::l_square || k == Tk::l_brace) j = match(j) - 1;
      else if (k == Tk::less) ++depth;
      else if (k == Tk::greater || k == Tk::greatergreater) {
        depth -= k == Tk::greater ? 1 : 2;
        if (depth <= 0) return j + 1;
      } else if (k == Tk::semi) {
        return j;
      }
    }
    return j;
  }

  void add(u32 k, std::string n, u32 at, u8 flags) {
    Item& it = out_.items[k];
    if (it.template_depth > 0 && (flags & name_flag::enumerator) == 0) flags |= name_flag::template_;
    // `template<…> class-key X<args>`, `template<…> T v<args>`: a partial specialization's name.
    if (it.specialization || (it.template_depth > 0 && kind(at + 1) == Tk::less && !n.starts_with("operator")))
      flags |= name_flag::specialization;
    it.names.push_back({std::move(n), at - it.begin, static_cast<u32>(it.names.size()), flags});
  }

  // The first token after attributes, `export`, `extern "C"` and template-heads.
  // A GNU attribute or asm label, `__attribute__((…))` or `__asm(…)`, which no name is in
  // (docs/EXTENSIONS.md).
  bool gnu_extension(u32 n) const {
    return kind(n + 1) == Tk::l_paren &&
           (kind(n) == Tk::kw_asm || is_name(n, "__attribute__") || is_name(n, "__asm") || is_name(n, "__asm__"));
  }

  u32 start_of(u32 k) const {
    const Item& it = out_.items[k];
    u32 j = it.begin;
    for (;;) {
      Tk t = kind(j);
      if (t == Tk::l_square && kind(j + 1) == Tk::l_square) j = match(j);
      else if ((t == Tk::kw_alignas && kind(j + 1) == Tk::l_paren) || gnu_extension(j)) j = match(j + 1);
      else if (t == Tk::kw_export) ++j;
      else if (t == Tk::kw_template && kind(j + 1) == Tk::less) j = match_angle(j + 1, it.end);
      else if (t == Tk::kw_extern && kind(j + 1) == Tk::string_lit) j += 2;
      else if (t == Tk::kw_requires) j = after_requires_clause(j + 1, it.end);
      else break;
    }
    return j;
  }

  // After a requires-clause's constraint-logical-or-expression ([temp.pre]): primaries joined by
  // `&&` and `||`.
  u32 after_requires_clause(u32 j, u32 limit) const {
    for (;;) {
      if (kind(j) == Tk::l_paren) {
        j = match(j);
      } else if (kind(j) == Tk::kw_requires) {
        ++j;
        if (kind(j) == Tk::l_paren) j = match(j);
        if (kind(j) == Tk::l_brace) j = match(j);
      } else if (kind(j) == Tk::kw_true || kind(j) == Tk::kw_false) {
        ++j;
      } else {
        while (kind(j) == Tk::identifier || kind(j) == Tk::coloncolon) {
          ++j;
          if (kind(j) == Tk::less) j = match_angle(j, limit);
        }
      }
      if (kind(j) != Tk::ampamp && kind(j) != Tk::pipepipe) return j;
      ++j;
    }
  }

  // `namespace a::b::c` (C++17, [namespace.def]) declares each component in the one before it, the
  // first in the enclosing namespace.
  void namespace_names(u32 k) {
    Item& it = out_.items[k];
    for (u32 n = it.begin; n < it.brace_begin; ++n) {
      // Past attributes: `[[…]]`, and GNU `__attribute__((…))` (docs/EXTENSIONS.md).
      if (kind(n) == Tk::l_square || (kind(n) == Tk::identifier && kind(n + 1) == Tk::l_paren)) {
        n = match(kind(n) == Tk::l_square ? n : n + 1) - 1;  // the loop steps past the closer
        continue;
      }
      if (kind(n) == Tk::identifier) add(k, std::string(name(n)), n, name_flag::namespace_ | name_flag::definition);
    }
  }

  void class_names(u32 k) {
    Item& it = out_.items[k];
    u32 s = start_of(k);
    bool friend_ = false;
    bool typedef_ = false;  // `typedef struct { … } T;`: the declarators are typedef-names
    u32 key = s;
    while (key < it.brace_begin && kind(key) != Tk::kw_class && kind(key) != Tk::kw_struct &&
           kind(key) != Tk::kw_union && kind(key) != Tk::kw_enum) {
      friend_ = friend_ || kind(key) == Tk::kw_friend;
      typedef_ = typedef_ || kind(key) == Tk::kw_typedef;
      ++key;
    }
    bool is_enum = kind(key) == Tk::kw_enum;
    bool scoped = is_enum && (kind(key + 1) == Tk::kw_class || kind(key + 1) == Tk::kw_struct);
    u32 head = none;
    u32 qual = none;
    int angle = 0;
    for (u32 n = key + 1; n < it.brace_begin; ++n) {
      Tk t = kind(n);
      if (t == Tk::l_square || t == Tk::l_paren) n = match(n) - 1;
      else if (t == Tk::less) ++angle;
      else if (t == Tk::greater) --angle;
      else if (t == Tk::colon && angle <= 0) break;
      else if (t == Tk::identifier && angle <= 0 && !is_name(n, "final")) {
        if (kind(n + 1) == Tk::coloncolon) {
          if (qual == none) qual = n;
        } else {
          head = n;
        }
      }
    }
    // `typedef struct { … } T;`: the class has T for its name ([dcl.typedef]/9), as sema names it.
    if (head == none && typedef_ && kind(it.brace_end) == Tk::identifier) head = it.brace_end;
    if (head != none) {
      if (qual != none) {
        it.qualifier_begin = qual;
        it.qualifier_end = head;
      }
      add(k, std::string(name(head)), head,
          name_flag::type | name_flag::definition | (friend_ ? name_flag::friend_ : 0));
    } else if (!is_enum && it.unnamed_type_ordinal != none) {
      add(k, {}, key, name_flag::type | name_flag::definition);
    }
    if (is_enum) {
      // Enumerators: the identifier at the start of each comma-separated entry.
      bool entry = true;
      for (u32 n = it.brace_begin + 1; n + 1 < it.brace_end; ++n) {
        Tk t = kind(n);
        if (t == Tk::l_paren || t == Tk::l_square || t == Tk::l_brace) {
          n = match(n) - 1;
          entry = false;
        } else if (t == Tk::comma) {
          entry = true;
        } else if (entry && t == Tk::identifier) {
          add(k, std::string(name(n)), n, name_flag::definition | (scoped ? 0 : name_flag::enumerator));
          entry = false;
        } else {
          entry = entry && t == Tk::l_square;  // attributes before an enumerator
        }
      }
    }
    // Declarators after the body: `struct S { … } s, *p;`.
    if (kind(it.brace_end) != Tk::semi && it.brace_end < it.end) {
      declarators(k, it.brace_end, it.end, Flags{.type = typedef_, .definition = !typedef_});
    }
  }

  struct Flags {
    bool type = false;
    bool friend_ = false;
    bool definition = false;
    bool unknown = false;
  };

  // A declaration's names: aliases, using-declarations, typedefs, forward class declarations,
  // concepts, and declarators.
  void declaration_names(u32 k, u32 s, u32 limit) {
    Item& it = out_.items[k];
    if (limit > it.begin && kind(limit - 1) == Tk::semi && !it.has_body()) --limit;
    if (s >= limit) return;
    Tk t = kind(s);
    if (t == Tk::kw_static_assert || t == Tk::kw_asm) return;
    if (t == Tk::kw_using) {
      if (kind(s + 1) == Tk::kw_namespace || kind(s + 1) == Tk::kw_enum) return;
      u32 n = s + 1;
      if (kind(n) == Tk::identifier) {
        u32 a = n + 1;
        while (kind(a) == Tk::l_square && kind(a + 1) == Tk::l_square) a = match(a);
        if (kind(a) == Tk::equal) {
          add(k, std::string(name(n)), n, name_flag::type);
          return;
        }
      }
      // A using-declaration: the last name of each declarator.
      u32 last = none;
      for (u32 j = s + 1; j <= limit; ++j) {
        if (j == limit || kind(j) == Tk::comma) {
          if (last != none) add(k, std::string(name(last)), last, name_flag::unknown);
          last = none;
        } else if (kind(j) == Tk::identifier) {
          last = j;
        } else if (kind(j) == Tk::less) {
          j = match_angle(j, limit) - 1;
        }
      }
      return;
    }
    if (t == Tk::kw_concept) {
      if (kind(s + 1) == Tk::identifier) add(k, std::string(name(s + 1)), s + 1, name_flag::definition);
      return;
    }
    Flags f;
    bool is_extern = false;
    bool is_static = false;
    bool is_inline = false;
    bool has_init = false;
    // Decl-specifiers that change what the names are.
    for (u32 j = s; j < limit; ++j) {
      Tk d = kind(j);
      if (d == Tk::l_paren || d == Tk::l_square || d == Tk::l_brace) {
        if (d == Tk::l_brace) has_init = true;
        j = match(j) - 1;
      } else if (d == Tk::equal) {
        has_init = true;
        break;
      }
      f.type = f.type || d == Tk::kw_typedef;
      f.friend_ = f.friend_ || d == Tk::kw_friend;
      is_extern = is_extern || d == Tk::kw_extern;
      is_static = is_static || d == Tk::kw_static;
      is_inline = is_inline || d == Tk::kw_inline || d == Tk::kw_constexpr;
    }
    // Standalone class declarations and opaque-enum-declarations declare a type name.
    u32 key = s;
    while (key < limit && (kind(key) == Tk::kw_friend || kind(key) == Tk::kw_extern)) ++key;
    if (is_class_key(kind(key)) || kind(key) == Tk::kw_enum) {
      u32 n = key + 1;
      if (kind(key) == Tk::kw_enum) {
        if (kind(n) == Tk::kw_class || kind(n) == Tk::kw_struct) ++n;
        while ((kind(n) == Tk::l_square && kind(n + 1) == Tk::l_square) ||
               (is_name(n, "__attribute__") && kind(n + 1) == Tk::l_paren))
          n = match(kind(n) == Tk::l_square ? n : n + 1);
      }
      u32 head = none;
      for (; n < limit && (kind(n) == Tk::identifier || kind(n) == Tk::coloncolon); ++n) {
        if (kind(n) == Tk::identifier) head = n;
      }
      if (head != none && (n == limit || kind(n) == Tk::colon || kind(n) == Tk::less)) {
        add(k, std::string(name(head)), head, name_flag::type | (f.friend_ ? name_flag::friend_ : 0));
        return;
      }
    }
    bool in_class = it.parent != none && out_.items[it.parent].kind == ItemKind::class_def;
    f.definition = it.has_body() || !(f.type || (is_extern && !has_init) || (in_class && is_static && !is_inline));
    declarators(k, s, limit, f);
  }

  // The declarator-ids of the init-declarators in [s, limit): split at depth-0 commas; template
  // arguments count as brackets outside initializers.
  void declarators(u32 k, u32 s, u32 limit, Flags f) {
    u32 begin = s;
    bool init = false;
    bool first = true;
    for (u32 j = s; j <= limit; ++j) {
      Tk t = j < limit ? kind(j) : Tk::semi;
      if (j == limit || (t == Tk::comma)) {
        declarator(k, begin, j, f, first);
        first = false;
        begin = j + 1;
        init = false;
      } else if (t == Tk::l_paren || t == Tk::l_square || t == Tk::l_brace) {
        j = std::min(match(j), limit) - 1;
      } else if (t == Tk::equal) {
        init = true;
      } else if (t == Tk::less && !init && j > s && (kind(j - 1) == Tk::identifier || kind(j - 1) == Tk::greater)) {
        j = match_angle(j, limit) - 1;
      } else if (t == Tk::kw_operator) {
        ++j;  // `operator,` and `operator<` are no separators
      }
    }
  }

  void declarator(u32 k, u32 from, u32 to, Flags f, bool first) {
    u32 cand = none;
    u32 cand_end = none;
    std::string spelled;
    u32 qual = none;
    u32 qual_begin = none;
    u32 qual_end = none;
    bool function = false;
    u32 n = from;
    while (n < to) {
      Tk t = kind(n);
      if (t == Tk::l_square && kind(n + 1) == Tk::l_square) {
        n = match(n);
      } else if (((t == Tk::kw_decltype || t == Tk::kw_alignas || t == Tk::kw_noexcept || t == Tk::kw_explicit ||
                   t == Tk::kw_sizeof) &&
                  kind(n + 1) == Tk::l_paren) ||
                 gnu_extension(n) || (transform_trait(n) && kind(n + 1) == Tk::l_paren)) {
        n = match(n + 1);
      } else if (t == Tk::identifier || t == Tk::coloncolon) {
        if (t == Tk::coloncolon || kind(n + 1) == Tk::coloncolon || (kind(n + 1) == Tk::less && angle_then_scope(n + 1, to))) {
          if (qual == none) qual = n;
          n = kind(n + 1) == Tk::less ? match_angle(n + 1, to) : n + 1;
          continue;
        }
        cand = n;
        spelled = name(n);
        qual_begin = qual;
        qual_end = n;
        qual = none;
        n = kind(n + 1) == Tk::less ? match_angle(n + 1, to) : n + 1;
        cand_end = n;
      } else if (t == Tk::tilde && kind(n + 1) == Tk::identifier) {
        cand = n;
        spelled = "~" + std::string(name(n + 1));
        qual_begin = qual;
        qual_end = n;
        qual = none;
        n += 2;
        cand_end = n;
      } else if (t == Tk::kw_operator) {
        cand = n;
        spelled = "operator";
        qual_begin = qual;
        qual_end = n;
        qual = none;
        ++n;
        if ((kind(n) == Tk::l_paren && kind(n + 1) == Tk::r_paren) ||
            (kind(n) == Tk::l_square && kind(n + 1) == Tk::r_square)) {
          spelled += kind(n) == Tk::l_paren ? "()" : "[]";
          n += 2;
        }
        while (n < to && kind(n) != Tk::l_paren) {
          Tk o = kind(n);
          bool word = o == Tk::identifier || lex::is_keyword(o) || lex::is_literal(o);
          if (word && spelled.back() != 'r' && spelled != "operator") spelled += ' ';
          if (word && spelled == "operator" && o != Tk::string_lit) spelled += ' ';
          spelled += r_.spelling(r_.tokens[n]);
          ++n;
        }
        cand_end = n;
      } else if (t == Tk::l_paren) {
        // A ptr-operator after `(` starts a grouped declarator, including `T (C::*p)(…)`.
        bool ptr = kind(n + 1) == Tk::star || kind(n + 1) == Tk::amp || kind(n + 1) == Tk::ampamp || member_ptr_ahead(n + 1, to);
        if (cand != none && n == cand_end && !ptr) {
          function = true;
          break;
        }
        // A grouping parenthesis: `(*fp)(int)`.
        u32 close = match(n);
        u32 before = static_cast<u32>(out_.items[k].names.size());
        declarator(k, n + 1, close - 1, f, first);
        if (out_.items[k].names.size() > before) return;
        n = close;
      } else if (t == Tk::l_square || t == Tk::equal || t == Tk::l_brace || t == Tk::colon || t == Tk::semi) {
        break;
      } else {
        // The qualifier belongs to `C::*`, not to the name this declarator introduces.
        if (t == Tk::star && n > from && kind(n - 1) == Tk::coloncolon) qual = none;
        if (t == Tk::less) {
          n = match_angle(n, to);
          continue;
        }
        ++n;
      }
    }
    if (cand == none) return;
    if (first && qual_begin != none) {
      out_.items[k].qualifier_begin = qual_begin;
      out_.items[k].qualifier_end = qual_end;
    }
    u8 flags = (f.type ? name_flag::type : 0) | (f.friend_ ? name_flag::friend_ : 0) |
               (f.unknown ? name_flag::unknown : 0);
    if (f.definition && !(function && !out_.items[k].has_body())) flags |= name_flag::definition;
    add(k, std::move(spelled), cand, flags);
  }

  // Whether the template arguments at `j` are followed by `::`: a qualifier `A<T>::`.
  bool angle_then_scope(u32 j, u32 to) const { return kind(match_angle(j, to)) == Tk::coloncolon; }

  bool member_ptr_ahead(u32 j, u32 to) const {
    u32 start = j;
    if (kind(j) == Tk::kw_decltype && kind(j + 1) == Tk::l_paren) {
      j = match(j + 1);
      if (kind(j) != Tk::coloncolon) return false;
      ++j;
    } else if (kind(j) == Tk::coloncolon) {
      ++j;
    }
    while (j < to && (kind(j) == Tk::identifier || kind(j) == Tk::kw_template)) {
      if (kind(j) == Tk::kw_template) ++j;
      if (kind(j) != Tk::identifier) break;
      u32 after = kind(j + 1) == Tk::less ? match_angle(j + 1, to) : j + 1;
      if (kind(after) != Tk::coloncolon) break;
      j = after + 1;
    }
    return j != start && kind(j) == Tk::star;
  }

  const pp::Region& r_;
  Items& out_;
};

// The tokens of an item's declaration part: everything but its body, the bodies of the members
// of a class, and the members of a namespace, linkage or export block.
template <class F>
void decl_tokens(const Items& items, u32 k, F&& f) {
  const Item& it = items.items[k];
  std::vector<std::pair<u32, u32>> skip;
  if (it.has_body()) skip.push_back({it.body_begin, it.body_end});
  if (it.kind == ItemKind::namespace_def || it.kind == ItemKind::linkage_block || it.kind == ItemKind::export_block) {
    skip.push_back({it.brace_begin + 1, it.brace_end});
  } else if (it.kind == ItemKind::class_def) {
    std::vector<u32> stack(it.members.begin(), it.members.end());
    while (!stack.empty()) {
      const Item& m = items.items[stack.back()];
      stack.pop_back();
      if (m.has_body()) skip.push_back({m.body_begin, m.body_end});
      stack.insert(stack.end(), m.members.begin(), m.members.end());
    }
  }
  std::ranges::sort(skip);
  std::size_t s = 0;
  for (u32 j = it.begin; j < it.end; ++j) {
    while (s < skip.size() && skip[s].second <= j) ++s;
    if (s < skip.size() && skip[s].first <= j) {
      j = skip[s].second - 1;
      continue;
    }
    f(j);
  }
}

constexpr Fp128 item_seed = fingerprint128(std::string_view("ycxx.items.ItemId"));
constexpr Fp128 body_seed = fingerprint128(std::string_view("ycxx.items.BodyId"));

}  // namespace ycxx::items

export namespace ycxx::items {

void hash_token(Hasher& h, const pp::Region& r, Tok t) {
  h.uint(static_cast<std::uint8_t>(t.kind));
  h.uint(static_cast<std::uint8_t>(t.flags & tok_flag::continued));
  if (t.kind == Tk::identifier || lex::is_literal(t.kind)) {
    std::string_view s = r.spelling(t);
    h.uint(static_cast<std::uint64_t>(s.size()));
    h.bytes(s);
  }
}

Items split(const pp::Region& r, Fp128 file) {
  Items out;
  Splitter(r, out).top();
  Names names(r, out);
  for (u32 k : out.top) names.run(k);
  // Ids: siblings in order, so an ordinal counts the earlier ones with the same hash.
  auto ids = [&](this auto& self, const std::vector<u32>& list) -> void {
    std::map<std::pair<std::uint64_t, std::uint64_t>, u32> seen;
    for (u32 k : list) {
      Hasher h;
      h.fp(item_seed).fp(file);
      h.uint(static_cast<std::uint64_t>(out.items[k].scope.size())).bytes(out.items[k].scope);
      if (out.items[k].unnamed_type_ordinal != none) h.uint(out.items[k].unnamed_type_ordinal);
      decl_tokens(out, k, [&](u32 j) { hash_token(h, r, r.tokens[j]); });
      Fp128 base = h.finish();
      Item& it = out.items[k];
      it.ordinal = seen[{base.lo, base.hi}]++;
      it.id = Hasher().fp(base).uint(it.ordinal).finish();
      if (it.has_body()) {
        Hasher b;
        b.fp(body_seed);
        for (u32 j = it.body_begin; j < it.body_end; ++j) hash_token(b, r, r.tokens[j]);
        it.body = b.finish();
      }
      self(it.members);
    }
  };
  ids(out.top);
  return out;
}

engine::query_task<Items> RegionItems::run(engine::Ctx& cx, pp::FileId file) {
  const pp::Region& r = co_await cx.fetch<pp::Pp>(file);
  co_return split(r, fingerprint(file));
}

}  // namespace ycxx::items
