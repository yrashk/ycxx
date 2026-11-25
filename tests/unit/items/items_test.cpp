// The item splitter (#18): item boundaries, declaration parts and bodies, nested item lists,
// skeleton names (ast.md §3.1), and the identity properties of query-engine.md §B1.2.
import std;
import ycxx.engine;
import ycxx.items;
import ycxx.unit;

using namespace ycxx;
using items::Item;
using items::ItemKind;
using items::Items;
using ycxx::unit::check;
namespace nf = items::name_flag;

namespace {

struct Split {
  pp::Region region;
  Items items;
  const Item& operator[](std::size_t k) const { return items.items[k]; }
  const Item& top(std::size_t k) const { return items.items[items.top[k]]; }
  std::size_t size() const { return items.top.size(); }
};

Split split(std::string_view text) {
  auto p = pp::preprocess(text);
  check(p.diags.empty(), "the text preprocesses cleanly");
  Split s{p.region, {}};
  s.items = items::split(s.region, fingerprint(pp::FileId{1}));
  return s;
}

std::vector<std::string> names(const Item& it) {
  std::vector<std::string> out;
  for (const auto& n : it.names) out.push_back(n.name);
  return out;
}

std::vector<std::string> v(std::initializer_list<std::string_view> xs) { return {xs.begin(), xs.end()}; }

std::string tokens(const Split& s, std::uint32_t begin, std::uint32_t end) {
  std::string out;
  for (std::uint32_t j = begin; j < end; ++j) {
    if (!out.empty()) out += ' ';
    out += s.region.spelling(s.region.tokens[j]);
  }
  return out;
}

void alpha() {
  auto s = split("int main() { return 42; }");
  check(s.size() == 1 && s.top(0).kind == ItemKind::function, "α0: one function");
  check(names(s.top(0)) == v({"main"}) && (s.top(0).names[0].flags & nf::definition), "main is defined");
  check(tokens(s, s.top(0).body_begin, s.top(0).body_end) == "{ return 42 ; }", "the body");
  check(s.top(0).decl_length() == 4 && s.top(0).names[0].offset == 1, "declaration part and name offset");

  auto c = split("int f(int);\nint x = 1, y;\nextern int z;\nextern \"C\" int puts(const char*);\n"
                 "int f(int n) { if (n) { return f(n - 1); } return 0; }\n;");
  check(c.size() == 6, "C0: six items");
  check(c.top(0).kind == ItemKind::declaration && names(c.top(0)) == v({"f"}) &&
            !(c.top(0).names[0].flags & nf::definition),
        "a function declaration");
  check(names(c.top(1)) == v({"x", "y"}) && (c.top(1).names[1].flags & nf::definition), "variables");
  check(names(c.top(2)) == v({"z"}) && !(c.top(2).names[0].flags & nf::definition), "extern is no definition");
  check(names(c.top(3)) == v({"puts"}), "extern \"C\" declaration");
  check(c.top(4).kind == ItemKind::function && c.top(4).body_end == c.top(4).end, "a body with nested braces");
  check(c.top(5).kind == ItemKind::empty, "an empty declaration");
}

void nesting() {
  auto s = split("extern \"C\" { int a; void b(); }\n"
                 "namespace N { int a; namespace M { void f() {} } }\n"
                 "namespace A::B { int c; }\nnamespace { int d; }\nnamespace X = N;\ninline namespace I {}\n"
                 "export { int e; }");
  check(s.size() == 7, "seven top-level items");
  check(s.top(0).kind == ItemKind::linkage_block && s.top(0).members.size() == 2, "linkage block");
  check(s[s.top(0).members[0]].scope.empty(), "a linkage block adds no scope");
  const Item& n = s.top(1);
  check(n.kind == ItemKind::namespace_def && names(n) == v({"N"}) && (n.names[0].flags & nf::namespace_),
        "namespace N");
  const Item& m = s[n.members[1]];
  check(m.scope == "N" && names(m) == v({"M"}) && s[m.members[0]].scope == "N::M", "nested scopes");
  // `namespace A::B` declares A, then B in A ([namespace.def], C++17).
  check(s.top(2).kind == ItemKind::namespace_def && names(s.top(2)) == v({"A", "B"}) &&
            s[s.top(2).members[0]].scope == "A::B",
        "namespace A::B");
  check(s[s.top(3).members[0]].scope == "(anonymous)", "unnamed namespace");
  check(s.top(4).kind == ItemKind::declaration, "a namespace alias is a declaration");
  check(s.top(5).kind == ItemKind::namespace_def && s.top(5).members.empty(), "inline namespace");
  check(s.top(6).kind == ItemKind::export_block && s.top(6).members.size() == 1, "export block");
  // Preorder: members follow their parent.
  check(s.items.top[1] + 1 == n.members[0], "preorder");
}

void classes() {
  auto s = split("struct S : B { int x; void f() { } S(); ~S(); private: int y = 1; static int z; "
                 "friend class Y; struct In { void g() {} }; } s, *p;");
  const Item& c = s.top(0);
  check(s.size() == 1 && c.kind == ItemKind::class_def, "a class definition with declarators");
  check(names(c) == v({"S", "s", "p"}) && (c.names[0].flags & nf::type), "class and declarator names");
  check(c.members.size() == 9, "members");
  std::vector<std::vector<std::string>> member_names;
  for (auto k : c.members) member_names.push_back(names(s[k]));
  check(member_names == std::vector{v({"x"}), v({"f"}), v({"S"}), v({"~S"}), v({}), v({"y"}), v({"z"}), v({"Y"}), v({"In"})},
        "member names");
  check(s[c.members[1]].kind == ItemKind::function && s[c.members[4]].kind == ItemKind::access_spec, "member kinds");
  check(!(s[c.members[6]].names[0].flags & nf::definition), "a static data member is declared, not defined");
  check(s[c.members[7]].names[0].flags & nf::friend_, "friend");
  check(s[c.members[0]].scope == "S", "members are in the class scope");
  // The nested class's members are nested again.
  auto t = split("struct S { struct In { void g() {} }; };");
  const Item& in = t[t.top(0).members[0]];
  check(in.kind == ItemKind::class_def && in.scope == "S" && t[in.members[0]].scope == "S::In", "nested class");
  // Not class definitions.
  auto d = split("struct S s{1}; struct T* f() { return 0; } class X; enum class F : int;");
  check(d.top(0).kind == ItemKind::declaration && names(d.top(0)) == v({"s"}) && d.top(0).members.empty() &&
            tokens(d, d.top(0).begin, d.top(0).end) == "struct S s { 1 } ;",
        "struct S s{1} keeps its initializer in the declaration");
  check(d.top(1).kind == ItemKind::function && names(d.top(1)) == v({"f"}), "a function returning struct T*");
  check(names(d.top(2)) == v({"X"}) && (d.top(2).names[0].flags & nf::type), "forward declaration");
  check(names(d.top(3)) == v({"F"}), "opaque enum declaration");
  // Enums.
  auto e = split("enum E { A, B = (1, 2), C } e; enum class F : int { X, Y };");
  check(e.top(0).kind == ItemKind::enum_def && names(e.top(0)) == v({"E", "A", "B", "C", "e"}), "enum names");
  check(e.top(0).names[1].flags & nf::enumerator, "unscoped enumerator");
  check(names(e.top(1)) == v({"F", "X", "Y"}) && !(e.top(1).names[1].flags & nf::enumerator), "scoped enumerators");
  // Class templates and partial specializations.
  auto p = split("template<class T> struct V<T*> final : Base<(1 > 2)> { };");
  check(p.top(0).kind == ItemKind::class_def && names(p.top(0)) == v({"V"}) && p.top(0).template_depth == 1,
        "partial specialization");
}

void braced_specialization_heads() {
  struct Head {
    std::string_view argument;
    std::string_view base_clause;
  };
  for (auto [arg, base] : {Head{"Key{1}", ""}, Head{"(Key{1})", ""}, Head{"false?one:Key{1}", ""},
                          Head{"Key{high>low?high:low}", ""}, Head{"Tag<Key{high>>1}>", " : Base"}}) {
    auto s = split(std::format("namespace N {{ template<> struct Pick<{}>{} {{ using type = long; }}; int after; }}",
                               arg, base));
    check(s.size() == 1 && s.top(0).members.size() == 2, arg);
    if (s.size() != 1 || s.top(0).members.size() != 2) continue;
    const auto& n = s.top(0);
    const auto& c = s[n.members[0]];
    const auto& after = s[n.members[1]];
    check(c.kind == ItemKind::class_def && c.specialization && c.template_depth == 0 &&
              c.parent == s.items.top[0] && c.scope == "N",
          std::format("{} defines an explicit specialization in N", arg));
    check(names(c) == v({"Pick"}) && c.names[0].offset == 4 &&
              (c.names[0].flags & (nf::type | nf::definition)) == (nf::type | nf::definition),
          std::format("{} keeps Pick as the defined type name", arg));
    check(c.begin == n.brace_begin + 1 && c.begin < c.brace_begin && c.brace_begin < c.brace_end &&
              c.end == c.brace_end + 1 && c.end <= n.brace_end &&
              tokens(s, c.brace_begin, c.end) == "{ using type = long ; } ;",
          std::format("{} starts the class body at the member list", arg));
    check(c.members.size() == 1, std::format("{} contributes only the real class member", arg));
    if (c.members.size() == 1) {
      const auto& m = s[c.members[0]];
      auto scope = m.scope;
      scope.erase(std::remove(scope.begin(), scope.end(), ' '), scope.end());
      check(names(m) == v({"type"}) && m.parent == n.members[0] &&
                m.begin == c.brace_begin + 1 && m.end + 1 == c.brace_end &&
                scope == std::format("N::Pick<{}>", arg),
            std::format("{} keeps the complete head in its member scope", arg));
    }
    check(after.begin == c.end && names(after) == v({"after"}) &&
              after.parent == s.items.top[0] && after.scope == "N",
          std::format("{} leaves the following declaration in N", arg));
  }
}

void functions_and_bodies() {
  auto s = split("S::S() : x(1), y{2}, Bs{}... { }\n"
                 "void f() try { } catch (int) { } catch (...) { }\n"
                 "struct T { T(const T&) = default; void g() = delete(\"no\"); virtual void h() = 0; "
                 "T& operator=(T&&) = default; bool operator==(const T&) const; operator int() const { return 0; } };\n"
                 "int operator\"\"_x(unsigned long long);\n"
                 "auto k() -> int { return 1; }\n"
                 "void (*fp)(int);\nint (*arr)[3];\n");
  const Item& ctor = s.top(0);
  check(ctor.kind == ItemKind::function && names(ctor) == v({"S"}) && tokens(s, ctor.qualifier_begin, ctor.qualifier_end) == "S ::",
        "constructor with its qualifier");
  check(tokens(s, ctor.body_begin, ctor.body_end) == ": x ( 1 ) , y { 2 } , Bs { } ... { }", "ctor-initializer in body");
  const Item& tryf = s.top(1);
  check(tryf.kind == ItemKind::function && tokens(s, tryf.body_begin, tryf.body_begin + 1) == "try" &&
            tryf.body_end == tryf.end && s.region.tokens[tryf.end - 1].kind == lex::Tk::r_brace,
        "function-try-block with handlers");
  const Item& t = s.top(2);
  check(t.members.size() == 6, "six members");
  check(s[t.members[0]].kind == ItemKind::function && tokens(s, s[t.members[0]].body_begin, s[t.members[0]].body_end) == "= default ;",
        "= default is a body");
  check(s[t.members[1]].kind == ItemKind::function, "= delete(\"no\") is a body");
  check(s[t.members[2]].kind == ItemKind::declaration && names(s[t.members[2]]) == v({"h"}), "= 0 is no body");
  check(names(s[t.members[3]]) == v({"operator="}) && s[t.members[3]].kind == ItemKind::function, "operator=");
  check(names(s[t.members[4]]) == v({"operator=="}), "operator==");
  check(names(s[t.members[5]]) == v({"operator int"}) && s[t.members[5]].kind == ItemKind::function,
        "conversion function");
  check(names(s.top(3)) == v({"operator\"\"_x"}), "literal operator");
  check(s.top(4).kind == ItemKind::function && names(s.top(4)) == v({"k"}), "trailing return type");
  check(names(s.top(5)) == v({"fp"}) && names(s.top(6)) == v({"arr"}), "grouping parentheses");
}

void initializers_and_templates() {
  auto s = split("auto l = [] { return 1; };\nint a[] = {1, 2};\nint b{3};\n"
                 "std::map<int, int> m, n;\nint x = a < b, y = c > d;\n"
                 "template<class T> requires requires (T t) { t.f(); } && C<T> void g(T) { }\n"
                 "template<class T> void h() requires (sizeof(T) > 1) { }\n"
                 "template<class T> concept C = requires { typename T::type; };\n"
                 "template<> template<class U> void V<int>::w(U) { }\n"
                 "template<class T, int N = (1 > 2)> struct Q;\n"
                 "consteval { }\n");
  check(s.size() == 11, "eleven items");
  check(s.top(0).kind == ItemKind::declaration && names(s.top(0)) == v({"l"}), "a lambda initializer");
  check(names(s.top(1)) == v({"a"}) && names(s.top(2)) == v({"b"}), "brace initializers");
  check(names(s.top(3)) == v({"m", "n"}), "template arguments are brackets in a declarator");
  check(names(s.top(4)) == v({"x", "y"}), "< and > in an initializer are operators");
  check(s.top(5).kind == ItemKind::function && names(s.top(5)) == v({"g"}) && s.top(5).template_depth == 1,
        "requires requires (T t) { … } is no body");
  check(s.top(6).kind == ItemKind::function && names(s.top(6)) == v({"h"}), "requires (…) clause before a body");
  check(s.top(7).kind == ItemKind::declaration && names(s.top(7)) == v({"C"}), "concept");
  // `template<>` is no template-head ([temp.expl.spec]): one head, in an explicit specialization.
  check(s.top(8).template_depth == 1 && s.top(8).specialization && names(s.top(8)) == v({"w"}) &&
            tokens(s, s.top(8).qualifier_begin, s.top(8).qualifier_end) == "V < int > ::",
        "member template of a specialization");
  check(names(s.top(9)) == v({"Q"}) && (s.top(9).names[0].flags & nf::template_), "template-head with (1 > 2)");
  check(s.top(10).kind == ItemKind::consteval_block && s.top(10).has_body(), "consteval block");
}

void using_and_typedef() {
  auto s = split("using I = int; using N::f, N::g; using namespace N; using enum E; typedef int T, *P;");
  check(names(s.top(0)) == v({"I"}) && (s.top(0).names[0].flags & nf::type), "alias");
  check(names(s.top(1)) == v({"f", "g"}) && (s.top(1).names[0].flags & nf::unknown), "using-declarations");
  check(names(s.top(2)).empty() && names(s.top(3)).empty(), "using-directive and using-enum declare no name");
  check(names(s.top(4)) == v({"T", "P"}) && (s.top(4).names[1].flags & nf::type), "typedef");
}

void recovery() {
  auto s = split("int a; } int b;");
  check(s.size() == 3 && s.top(1).kind == ItemKind::stray && names(s.top(2)) == v({"b"}), "a stray }");
  auto t = split("void f() { if (x) {");
  check(t.size() == 1 && t.top(0).end + 1 == t.region.size(), "an unterminated body runs to the end");

  auto missing_angle = split("template<> struct Pick<Tag<Key{1}>; using sentinel = int;");
  check(missing_angle.size() >= 2 && names(missing_angle.top(missing_angle.size() - 1)) == v({"sentinel"}),
        "a missing outer > recovers at the semicolon");

  auto missing_semi = split("template<> struct Pick<Key{1}> { using type = long; } using sentinel = int;");
  check(missing_semi.size() == 2 && missing_semi.top(0).kind == ItemKind::class_def &&
            missing_semi.top(0).end == missing_semi.top(0).brace_end &&
            missing_semi.top(1).begin == missing_semi.top(0).end && names(missing_semi.top(1)) == v({"sentinel"}),
        "a missing semicolon after the class body preserves the following declaration");
}

void ids() {
  auto id = [](std::string_view text, std::size_t k = 0) { return split(text).top(k).id; };
  auto body = [](std::string_view text, std::size_t k = 0) { return split(text).top(k).body; };
  // A body edit keeps the id and changes the body (§B1.2).
  check(id("int f() { return 1; }") == id("int f() { return 2; }"), "body edits keep the id");
  check(body("int f() { return 1; }") != body("int f() { return 2; }"), "body edits change the body");
  check(body("int f() { return 1; }") == body("int g() { return 1; }"), "bodies are content-addressed");
  // A declaration edit changes it.
  check(id("void f(int);") != id("void f(int) noexcept;"), "declaration edits change the id");
  // Whitespace and comments change nothing; an alternative spelling neither.
  check(id("int  x ;") == id("int x;/* */") && id("bool b = x && y;") == id("bool b = x and y;"),
        "whitespace and spelling");
  // Adding an unrelated item changes no id.
  check(id("int a; int b;", 1) == id("int b;") && id("int a; int b;", 1) == id("int a; int c; int b;", 2),
        "unrelated items");
  // Redeclarations get ordinals.
  auto r = split("void f(int); void f(int);");
  check(r.top(0).ordinal == 0 && r.top(1).ordinal == 1 && r.top(0).id != r.top(1).id, "ordinals");
  // The scope path is part of the id.
  auto n = split("namespace A { int x; } namespace B { int x; }");
  check(n[n.top(0).members[0]].id != n[n.top(1).members[0]].id, "scope path");
  // A namespace's id ignores its members; a class's does not, but ignores member bodies.
  check(id("namespace N { int a; }") == id("namespace N { int b; }"), "namespace members are separate items");
  check(id("struct S { int a; };") != id("struct S { int b; };"), "class members are part of the class");
  check(id("struct S { int f() { return 1; } };") == id("struct S { int f() { return 2; } };"),
        "member bodies are not");
  // The file is part of the id.
  auto r1 = pp::preprocess("int x;").region;
  check(items::split(r1, fingerprint(pp::FileId{1})).items[0].id != items::split(r1, fingerprint(pp::FileId{2})).items[0].id,
        "file identity");
}

// Items tile the tokens: each list's items are contiguous and in order, members tile their
// parent's braces, and bodies lie inside their items. Checked on every prefix of a sample, so
// truncated input (unbalanced brackets, a missing `;`) is covered too.
bool tiles(const pp::Region& r, const Items& s, const std::vector<std::uint32_t>& list, std::uint32_t begin,
           std::uint32_t end) {
  std::uint32_t at = begin;
  for (auto k : list) {
    const Item& it = s.items[k];
    if (it.begin != at || it.end < it.begin || it.end > end) return false;
    if (it.has_body() && (it.body_begin < it.begin || it.body_end > it.end)) return false;
    if (!it.members.empty()) {
      // Members fill the braces, up to the `}` if there is one.
      bool closed = it.brace_end > it.brace_begin + 1 && r.tokens[it.brace_end - 1].kind == lex::Tk::r_brace &&
                    s.items[it.members.back()].end < it.brace_end;
      if (!tiles(r, s, it.members, it.brace_begin + 1, closed ? it.brace_end - 1 : it.brace_end)) return false;
    }
    at = it.end;
  }
  return at == end;
}

void robustness() {
  std::string sample =
      "namespace N { template<class T> struct S : B<T> { S() : x{1} {} int f() try { return 1; } catch (...) "
      "{ return 0; } private: int x = [] { return 2; }(); enum E { A, B } e; }; } extern \"C\" { int puts(const "
      "char*); } int main() { return N::S<int>{}.f(); } using I = int; template<class T> requires requires (T t) "
      "{ t; } void g(T) {} } ; struct { int a; } anon; consteval { } "
      "template<> struct Pick<Tag<(Key{1})>> { using type = long; }; int after;";
  std::size_t checked = 0;
  for (std::size_t n = 0; n <= sample.size(); ++n) {
    auto p = pp::preprocess(std::string_view(sample).substr(0, n));
    Items s = items::split(p.region, Fp128{});
    auto eof = static_cast<std::uint32_t>(p.region.size() - 1);
    check(tiles(p.region, s, s.top, 0, eof), "items tile every prefix");
    ++checked;
  }
  check(checked == sample.size() + 1, "every prefix");
}

void unnamed_type_scopes() {
  auto s = split("typedef struct { union { int x; } value, copy; union { int x; } spare; } A; "
                 "typedef struct { union { int x; } value; } B;");
  const Item& value = s[s.top(0).members[0]];
  const Item& spare = s[s.top(0).members[1]];
  const Item& other = s[s.top(1).members[0]];
  check(names(value) == v({"", "value", "copy"}) && value.unnamed_type_ordinal == 0 && spare.unnamed_type_ordinal == 1,
        "the unnamed union head occupies slot zero and one lexical type ordinal");
  check(s[value.members[0]].id != s[spare.members[0]].id && s[value.members[0]].id != s[other.members[0]].id,
        "identical field declarations in different unnamed unions have distinct ids");

  auto shifted = split("typedef struct { int prefix; union { int x; } spare; union { int x; } value, copy; } A;");
  const Item& moved = shifted[shifted.top(0).members[2]];
  check(moved.id != value.id && shifted[moved.members[0]].id != s[value.members[0]].id,
        "a changed lexical ordinal gives the union and its fields new ids");

  auto n = split("struct N { typedef union { int x; } Alias; enum { e }; union { int a; }; union { int b; } member; };");
  const auto& members = n.top(0).members;
  check(n[members[0]].unnamed_type_ordinal == items::none && n[members[1]].unnamed_type_ordinal == 0 &&
            n[members[2]].unnamed_type_ordinal == 1 && n[members[3]].unnamed_type_ordinal == 2,
        "ordinal counting includes unnamed enums and anonymous unions and excludes typedef linkage names");
}

void query() {
  engine::Db db;
  pp::FileId f{3};
  db.set<pp::SourceOf>(f, {"<file 3>", "", {}});
  db.set<pp::FileText>(f, "int main() { return 42; }");
  const Items& a = db.get<items::RegionItems>(f);
  check(a.top.size() == 1 && a.items[0].kind == ItemKind::function, "region_items");
  Fp128 id = a.items[0].id;
  db.set<pp::FileText>(f, "int main() { return 43; }");
  const Items& b = db.get<items::RegionItems>(f);
  check(b.items[0].id == id && b.items[0].body != Fp128{}, "the id survives a body edit through the engine");
}

}  // namespace

int main() {
  alpha();
  nesting();
  classes();
  braced_specialization_heads();
  functions_and_bodies();
  initializers_and_templates();
  using_and_typedef();
  recovery();
  ids();
  robustness();
  unnamed_type_scopes();
  query();
  return ycxx::unit::result("unit.items");
}
