// Phase 4 (ycxx.pp): include resolution, object-like replacement, the questions a region records,
// reuse of regions whose traces verify, and incremental equals cold over edit sequences
// (DECISIONS §4.2). `-E` output is compared with Clang's by the preprocess tests (tests/own/pp).
import std;
import ycxx.ast.target;
import ycxx.engine;
import ycxx.pp;
import ycxx.unit;

using namespace ycxx;
using pp::Tk;
using unit::check;

namespace {

// Files in memory.
class MemHost final : public pp::Host {
public:
  std::map<std::string, std::string> files;
  std::optional<std::string> read(const std::string& path) override {
    auto it = files.find(path);
    if (it == files.end()) return std::nullopt;
    return it->second;
  }
};

// A region's tokens spelled, space-separated.
std::string text(const pp::Region& r) {
  std::string out;
  for (std::size_t i = 0; i + 1 < r.tokens.size(); ++i) {
    if (!out.empty()) out += ' ';
    out += r.spelling(r.tokens[i]);
  }
  return out;
}

std::vector<lex::LexDiag> kinds(const pp::Preprocessed& p) {
  std::vector<lex::LexDiag> out;
  for (const pp::Diag& d : p.diags) out.push_back(d.kind);
  return out;
}

pp::Preprocessed run(pp::Session& s, MemHost& h, std::string_view main = "main.cpp") {
  return pp::preprocess(s, h.files.at(std::string(main)), main);
}

void includes() {
  MemHost h;
  h.files["dir/main.cpp"] = "#include \"a.h\"\n#include <b.h>\n#include <c.h>\nint m;\n";
  h.files["dir/a.h"] = "int a;\n#include \"sub/d.h\"\n";
  h.files["dir/sub/d.h"] = "int d;\n#include \"e.h\"\n";
  h.files["dir/sub/e.h"] = "int e;\n";
  h.files["inc1/b.h"] = "int b1;\n";
  h.files["inc1/c.h"] = "int c1;\n#include_next <c.h>\n";
  h.files["inc2/c.h"] = "int c2;\n";
  pp::Config cfg;
  cfg.angle_dirs = {"inc1", "inc2"};
  pp::Session s(h, cfg);
  auto p = run(s, h, "dir/main.cpp");
  check(p.diags.empty(), "includes resolve");
  check(text(p.region) == "int a ; int d ; int e ; int b1 ; int c1 ; int c2 ; int m ;", "included text in order");
  check(p.region.file_names ==
            std::vector<std::string>{"dir/main.cpp", "dir/a.h", "dir/sub/d.h", "dir/sub/e.h", "inc1/b.h", "inc1/c.h", "inc2/c.h"},
        "presumed names");
  check(p.region.locs[3].file == 2 && p.region.locs[3].offset == 0, "a header's token is located in its file");

  h.files["dir/main.cpp"] = "int x;\n#include \"missing.h\"\nint y;\n";
  auto miss = run(s, h, "dir/main.cpp");
  check(kinds(miss) == std::vector{lex::LexDiag::include_not_found}, "a missing header");
  check(miss.diags[0].offset == 16 && text(miss.region) == "int x ;", "is located at its name and stops phase 4");

  // [cpp.include]/4: the macro-replaced forms.
  h.files["dir/main.cpp"] = "#define Q \"a.h\"\n#define A <b.h>\n#include Q\n#include A\n";
  check(text(run(s, h, "dir/main.cpp").region) == "int a ; int d ; int e ; int b1 ;", "#include pp-tokens");
  h.files["dir/main.cpp"] = "#include\n#include x\n#include <b.h> junk\n";
  check(kinds(run(s, h, "dir/main.cpp")) ==
            std::vector{lex::LexDiag::include_expected, lex::LexDiag::include_expected, lex::LexDiag::extra_tokens},
        "malformed includes");

  // `#pragma once`: the second include is skipped.
  h.files["dir/o.h"] = "#pragma once\nint o;\n";
  h.files["dir/main.cpp"] = "#include \"o.h\"\n#include \"o.h\"\n#pragma once\n";
  auto once = run(s, h, "dir/main.cpp");
  check(text(once.region) == "int o ;" && kinds(once) == std::vector{lex::LexDiag::pragma_once_in_main}, "#pragma once");
}

void linux_system_includes() {
  pp::Config cfg;
  cfg.target = &ast::x86_64_linux;
  std::string include = "/usr/include";
  auto multiarch = include + "/x86_64-linux-gnu";
  auto darwin = pp::system_dirs(ast::aarch64_darwin);
  check(std::ranges::none_of(darwin, [&](const auto& dir) {
          return dir == include || dir == multiarch || dir.ends_with("/resources/linux/include");
        }),
        "Darwin does not search Linux's include directories");
  if (pp::native_target != cfg.target) {
    auto linux_dirs = pp::system_dirs(*cfg.target);
    check(!std::ranges::contains(linux_dirs, include) && !std::ranges::contains(linux_dirs, multiarch),
          "foreign Linux does not search native C include directories");
    return;
  }
  cfg.angle_dirs = {multiarch, "user"};
  cfg.system_from = 2;
  cfg = pp::with_system_dirs(std::move(cfg));
  check(cfg.system_from == 1 && cfg.angle_dirs.front() == "user",
        "an explicit -I for the multiarch directory stays at its system location");

  MemHost h;
  h.files["main.cpp"] = "#include <multiarch.h>\n";
  h.files[multiarch + "/multiarch.h"] = "int multiarch;\n#include_next <multiarch.h>\n";
  h.files[include + "/multiarch.h"] = "int fallback;\n";
  pp::Session s(h, cfg);
  auto p = run(s, h);
  check(p.diags.empty() && text(p.region) == "int multiarch ; int fallback ;",
        "the Linux multiarch header comes first and include_next reaches usr/include");
}

void linux_stddef() {
  pp::Config cfg;
  cfg.target = &ast::x86_64_linux;
  pp::DiskHost host;
  pp::Session s(host, pp::with_system_dirs(std::move(cfg)));
  auto partial = pp::preprocess(s, R"cpp(
#define __need_size_t
#include <stddef.h>
#if defined(__need_size_t) || defined(NULL) || defined(offsetof)
#error size_t request leaked other definitions
#endif
size_t first;
#define __need_NULL
#define __need_wchar_t
#include <stddef.h>
#if defined(__need_NULL) || defined(__need_wchar_t) || !defined(NULL) || defined(offsetof)
#error combined request was not isolated
#endif
void* pointer = NULL;
)cpp", "partial.cpp");
  check(partial.diags.empty() &&
            text(partial.region) == "typedef long unsigned int size_t ; size_t first ; void * pointer = nullptr ;",
        "libycxx's wrapper forwards each partial request to the Linux resource header");

  auto full = pp::preprocess(s, R"cpp(
#define __need_size_t
#include <stddef.h>
#include <cstddef>
#if !defined(NULL) || !defined(offsetof)
#error full stddef.h did not define its macros
#endif
#undef NULL
#define __need_NULL
#include <stddef.h>
#if defined(__need_NULL) || !defined(NULL)
#error a partial stddef.h include must work after the full include
#endif
)cpp", "full.cpp");
  check(full.diags.empty() && text(full.region).contains("} max_align_t ;"),
        "libycxx's full cstddef finds max_align_t after a partial stddef.h include");
}

void source_target() {
  for (auto target : {&ast::aarch64_darwin, &ast::x86_64_linux}) {
    pp::Source source{"main.cpp", {}, {}};
    source.config.target = target;
    engine::Sink bytes;
    engine::serialize(bytes, source);
    engine::Source in(bytes.bytes());
    auto restored = engine::deserialize<pp::Source>(in);
    check(!in.failed() && in.done() && restored.config.target == target,
          "a saved preprocessor source preserves its target");
  }
}

void object_like() {
  MemHost h;
  pp::Session s(h);
  auto pre = [&](std::string src) {
    h.files["main.cpp"] = std::move(src);
    return run(s, h);
  };
  check(text(pre("#define N 42\n#define M N + N\nint a = M;\n").region) == "int a = 42 + 42 ;", "replacement and rescanning");
  // [cpp.rescan]/2: a macro's own name is not replaced again, here or later.
  check(text(pre("#define A B\n#define B A\nA B\n#define S S + 1\nS\n").region) == "A B S + 1", "self-reference");
  check(text(pre("#define E\na E b\n").region) == "a b", "an empty replacement");
  check(text(pre("#define X 1\n#undef X\n#undef Y\nX\n").region) == "X", "#undef");
  auto redef = pre("#define R 1 + 2\n#define R 1  +  2\n#define R 1+2\n");
  check(kinds(redef) == std::vector{lex::LexDiag::macro_redefined}, "redefinition compares whitespace by presence");
  check(redef.diags[0].offset == 42, "located at the name");
  check(kinds(pre("#define\n#define 1\n#define defined\n#undef\n")) ==
            std::vector{lex::LexDiag::macro_name_missing, lex::LexDiag::macro_name_not_identifier,
                        lex::LexDiag::macro_name_defined, lex::LexDiag::macro_name_missing},
        "macro names");
  // The token keeps its definition's location, inside an expansion whose site is the use.
  auto loc = pre("#define V value\nint V;\n");
  const pp::Region& r = loc.region;
  check(r.locs[1].offset == 10 && r.locs[1].expansion == 1, "a replacement token is spelled in the definition");
  check(r.expansions[1].site_offset == 20 && r.expansions[1].definition_offset == 8 && r.name(r.tokens[1]) == "value",
        "its expansion");
  check(r.site(1).offset == 20, "its site");
}

void function_like() {
  MemHost h;
  pp::Session s(h);
  auto pre = [&](std::string src) {
    h.files["main.cpp"] = std::move(src);
    return run(s, h);
  };
  // [cpp.rescan]/2's example: f(2)(9) is 2*9*g.
  check(text(pre("#define f(a) a*g\n#define g(a) f(a)\nf(2)(9)\n").region) == "2 * 9 * g", "rescanning");
  check(text(pre("#define F(...) f(0 __VA_OPT__(,) __VA_ARGS__)\nF(a, b) F()\n").region) == "f ( 0 , a , b ) f ( 0 )",
        "__VA_OPT__");
  check(text(pre("#define t(x,y,z) x ## y ## z\nt(1,2,3) t(,4,5) t(,,)\n").region) == "123 45", "placemarkers");
  check(text(pre("#define S(x) #x\nS( a  \"b\\n\" ) S()\n").region) == "\"a \\\"b\\\\n\\\"\" \"\"", "stringizing");
  check(text(pre("#define F(a) a\nF\n+ F\n(1)\n").region) == "F + 1", "a name without ( is left alone");
  // An argument's tokens keep their spelling, in an argument expansion whose site leads to the
  // invocation's.
  auto loc = pre("#define ID(x) [x]\nint a = ID(v);\n");
  const pp::Region& r = loc.region;
  std::size_t v = 4;  // int a = [ v ] ;
  check(r.name(r.tokens[v]) == "v" && r.locs[v].offset == 29, "an argument token's spelling");
  check(r.expansions[r.locs[v].expansion].kind == pp::ExpansionKind::argument && r.site(v).offset == 26,
        "its expansion and site");
  check(kinds(pre("#define P(a,b) a##b\nP(/,/)\n")) == std::vector{lex::LexDiag::paste_invalid}, "an invalid paste");
  check(kinds(pre("#define J(a,b) a b\nJ(1) J(1,2,3)\n")) ==
            std::vector{lex::LexDiag::too_few_arguments, lex::LexDiag::too_many_arguments},
        "argument counts");
  // Found by review: an argument is replaced once, however often it is used ([cpp.subst]/1).
  check(kinds(pre("#define g(a) a\n#define F(...) __VA_OPT__(x) __VA_ARGS__ __VA_ARGS__\nF(g(1,2))\n")) ==
            std::vector{lex::LexDiag::too_many_arguments},
        "an argument replaced once");
  // A directive ends the search for `(`.
  check(text(pre("#define f(x) [x]\na f\n#undef f\n(1) b\n").region) == "a f ( 1 ) b", "a directive before (");
  check(kinds(pre("#define f(x) x\nf(1\n#include \"x.h\"\n)\n")) == std::vector{lex::LexDiag::directive_in_arguments},
        "#include in arguments");
  // Stringizing a raw string written across lines, and an invalid result.
  check(text(pre("#define s(x) #x\ns(R\"(a\nb)\")\n").region) == "\"R\\\"(a\\nb)\\\"\"", "a raw string stringized");
  check(kinds(pre("#define s(x) #x\ns(\\)\n")) == std::vector{lex::LexDiag::stringize_invalid}, "an invalid stringizing");
  check(kinds(pre("#define F(__VA_OPT__, ...)\n")) == std::vector{lex::LexDiag::macro_param_expected}, "__VA_OPT__ is no parameter");
  // A header's invocation of a macro the includer defines is reused where an equal definition is
  // elsewhere.
  h.files["use.h"] = "int u = TWICE(3);\n";
  h.files["a.cpp"] = "#define TWICE(x) ((x) * 2)\n#include \"use.h\"\n";
  h.files["b.cpp"] = "\n\n#define TWICE(x) ((x) * 2)\n#include \"use.h\"\n";
  run(s, h, "a.cpp");
  auto computed = s.stats.computed;
  auto b = run(s, h, "b.cpp");
  check(s.stats.computed == computed + 1 && text(b.region) == "int u = ( ( 3 ) * 2 ) ;", "a header reused across TUs");
  check(b.region.expansions[1].definition_offset == 10 && b.region.file_names[b.region.expansions[1].definition_file] == "b.cpp",
        "its expansion bound to this TU's definition");
}

// The questions a region asks of its entry state.
void traces() {
  MemHost h;
  h.files["h.h"] = "#define LOCAL 1\nint v = LOCAL + OUTER + Z;\n#undef Z\nint w = Z;\n";
  h.files["main.cpp"] = "#define Z 3\n#include \"h.h\"\n";
  pp::Session s(h);
  auto p = run(s, h);
  check(text(p.region) == "int v = 1 + OUTER + 3 ; int w = Z ;", "the header's text");
  auto fid = s.files.id("h.h");
  const std::shared_ptr<const pp::FileRegion>* header = nullptr;
  for (const auto& [key, entries] : s.cache)
    if (key.file == fid) header = &entries.front();
  check(header != nullptr, "the header's region is cached");
  if (!header) return;
  std::set<std::string> asked;
  for (const pp::Question& q : (*header)->trace)
    if (q.ask == pp::Ask::macro) asked.emplace(s.spellings.name(q.a));
  // Answered at entry: `int`, `v`, `OUTER`, `Z`, `w`, and LOCAL's absence (a redefinition would be
  // diagnosed). LOCAL's use and Z's after its #undef are the header's own.
  check(asked == std::set<std::string>{"LOCAL", "int", "v", "OUTER", "Z", "w"}, "macro questions are the entry state's");
  check((*header)->delta.size() == 2, "the delta: LOCAL defined, Z undefined");
}

// §B9's example: a header expands differently where its entry state differs, and is reused where
// it does not.
void reuse() {
  MemHost h;
  h.files["b.h"] = "int v = Z;\n";
  h.files["one.cpp"] = "#define Z 1\n#include \"b.h\"\n";
  h.files["two.cpp"] = "#define Z 2\n#include \"b.h\"\n";
  h.files["three.cpp"] = "#define Z 1\n#define Unrelated\n#include \"b.h\"\n";
  pp::Session s(h);
  check(text(run(s, h, "one.cpp").region) == "int v = 1 ;", "TU one");
  check(text(run(s, h, "two.cpp").region) == "int v = 2 ;", "TU two");
  check(s.stats.computed == 5 && s.stats.reused == 1, "b.h computed again for another Z");
  check(text(run(s, h, "three.cpp").region) == "int v = 1 ;", "TU three");
  check(s.stats.computed == 6 && s.stats.reused == 3, "b.h reused: its trace verifies");
  // The same TU again reuses the main file's region, and with it the tree.
  run(s, h, "one.cpp");
  check(s.stats.computed == 6 && s.stats.reused == 5, "a TU unchanged is reused whole");
  // A macro defined by a reused header is defined after it.
  h.files["def.h"] = "#define D 4\n";
  h.files["four.cpp"] = "#include \"def.h\"\nint d = D;\n";
  h.files["five.cpp"] = "#include \"def.h\"\nint e = D;\n";
  run(s, h, "four.cpp");
  check(text(run(s, h, "five.cpp").region) == "int e = 4 ;", "a reused region's delta applies");
}

// Conditionals, the guard summary and the counter.
void conditionals() {
  MemHost h;
  pp::Session s(h);
  auto pre = [&](std::string src) {
    h.files["main.cpp"] = std::move(src);
    return run(s, h);
  };
  check(text(pre("#if 1 + 1 == 2 && defined(__cplusplus)\na\n#elif 1/0\nb\n#else\nc\n#endif\n").region) == "a", "#if");
  check(text(pre("#if 0\n#bogus\n#if 1\nno\n#endif\n#elifdef __LINE__\nyes\n#endif\n").region) == "yes", "skipped groups");
  check(kinds(pre("#if 0\n'x\n#endif\n")) == std::vector{lex::LexDiag::unterminated_char}, "lexed all the same");
  check(kinds(pre("#if 1\n#else\n#else\n#endif\n#endif\n#if 1\n")) ==
            std::vector{lex::LexDiag::cond_else_after_else, lex::LexDiag::cond_endif_without_if, lex::LexDiag::cond_unterminated},
        "mismatched conditionals");
  check(text(pre("#if -1 < 0u\nno\n#else\nyes\n#endif\n#if 0 && 1/0\n#endif\n").region) == "yes", "unsigned, short-circuit");
  // A guarded header included again is an empty region with one question.
  h.files["g.h"] = "// guard\n#ifndef G\n#define G\nint g;\n#endif\n";
  pre("#include \"g.h\"\n");
  auto before = s.stats;
  auto twice = pre("#include \"g.h\"\n#include \"g.h\"\n");
  check(text(twice.region) == "int g ;" && s.stats.computed == before.computed + 1, "a guarded header, read once");
  // `#ifdef` asks whether, not what: a different definition of the name does not matter.
  h.files["d.h"] = "#ifdef D\nint d;\n#endif\n";
  pre("#define D 1\n#include \"d.h\"\n");
  auto c = s.stats.computed;
  check(text(pre("#define D 2\n#include \"d.h\"\n").region) == "int d ;" && s.stats.computed == c + 1, "defined(D) only");
  // __COUNTER__ continues across a reused region.
  h.files["c.h"] = "int a = __COUNTER__;\n";
  check(text(pre("#include \"c.h\"\n#include \"c.h\"\nint b = __COUNTER__;\n").region) == "int a = 0 ; int a = 1 ; int b = 2 ;",
        "the counter");
  check(text(pre("int z = __COUNTER__;\n#include \"c.h\"\n").region) == "int z = 0 ; int a = 1 ;", "a region's counter question");
  // Found by review: a directive outside the guarded group makes the file unguarded, and so does
  // a diagnostic its skipping would repeat.
  h.files["g1.h"] = "#ifndef G1\n#define G1\nint g1;\n#endif\n#define AFTER 42\n";
  check(text(pre("#include \"g1.h\"\n#undef AFTER\n#include \"g1.h\"\nAFTER\n").region) == "int g1 ; 42", "not a guard");
  h.files["g4.h"] = "#ifndef G4\n#define G4\n#endif extra\n";
  check(kinds(pre("#include \"g4.h\"\n#include \"g4.h\"\n")) ==
            std::vector{lex::LexDiag::extra_tokens, lex::LexDiag::extra_tokens},
        "a guarded file that says something is read again");
  // __LINE__: an argument's own line, or the `)` of the invocation whose replacement made it.
  check(text(pre("#define F(x) x\n#define H(x) x __LINE__\nF(\n__LINE__\n) F(H(\n1\n)\n)\n").region) == "4 1 7",
        "__LINE__ in arguments");
  check(text(pre("#if (1, 2) == 2\nc\n#endif\n").region) == "c", "a comma in parentheses");
  // #line
  auto l = pre("#line 10 \"x.cpp\"\nint a = __LINE__; const char* f = __FILE__;\n#line 1\n#error e\n");
  check(text(l.region) == "int a = 10 ; const char * f = \"x.cpp\" ;", "#line");
  pp::Message m = pp::message(s, s.files.id("main.cpp"), "main.cpp", l.diags.at(0));
  check(l.diags.size() == 1 && m.line == 1 && m.file == "x.cpp", "a diagnostic's presumed location");
}

// What a region's value depends on beyond its trace: whether it is a main file, and how deep it
// is nested (both found by review).
void roots_and_depth() {
  MemHost h;
  h.files["h.cpp"] = "#pragma once\nint h;\n";
  h.files["m.cpp"] = "#include \"h.cpp\"\n#include \"h.cpp\"\nint m;\n";
  pp::Session s(h);
  check(kinds(run(s, h, "h.cpp")) == std::vector{lex::LexDiag::pragma_once_in_main}, "#pragma once in the main file");
  auto m = run(s, h, "m.cpp");
  check(text(m.region) == "int h ; int m ;" && m.diags.empty(), "the same file as a header is another region");

  // A chain of 150 headers fits under the limit of 200 from a main file, but not below 60 more.
  for (int i = 1; i <= 150; ++i) h.files[std::format("c{}.h", i)] = i < 150 ? std::format("#include \"c{}.h\"\n", i + 1) : "int end;\n";
  for (int i = 1; i <= 60; ++i) h.files[std::format("d{}.h", i)] = std::format("#include \"{}.h\"\n", i < 60 ? std::format("d{}", i + 1) : "c1");
  h.files["a.cpp"] = "#include \"c1.h\"\n";
  h.files["b.cpp"] = "#include \"d1.h\"\n";
  check(text(run(s, h, "a.cpp").region) == "int end ;", "a deep chain");
  auto deep = run(s, h, "b.cpp");
  pp::Session cold(h);
  auto fresh = run(cold, h, "b.cpp");
  check(kinds(deep) == std::vector{lex::LexDiag::include_too_deep} && deep.region == fresh.region && deep.diags == fresh.diags,
        "a region is not reused deeper than it fits");
}

// Incremental equals cold (DECISIONS §4.2): after each edit, a session that has seen every earlier
// state gives the same region and diagnostics as a fresh session.
void incremental_equals_cold() {
  std::vector<std::map<std::string, std::string>> states = {
      {{"main.cpp", "#include \"a.h\"\n#include \"b.h\"\nint m = A + B;\n"},
       {"a.h", "#define A 1\n#include \"b.h\"\n"},
       {"b.h", "#pragma once\nint b = A;\n#define B 2\n"}},
      {{"b.h", "#pragma once\nint b = A;\n#define B 3\n"}},
      {{"a.h", "#define A 5\n"}},
      {{"main.cpp", "#define A 0\n#include \"b.h\"\n#include \"a.h\"\nint m = A + B;\n"}},
      {{"a.h", "#include \"b.h\"\n#undef B\n"}},
      {{"b.h", "int b = A;\n#define B A\n"}},
      {{"main.cpp", "#include \"a.h\"\n#include \"a.h\"\nB\n"}},
      {{"c.h", "int c;\n"}, {"a.h", "#include \"c.h\"\n#include \"missing.h\"\n"}},
      {{"missing.h", "int now_here;\n"}},
      {{"main.cpp", "#define F(x) [x A]\n#include \"a.h\"\nF(B) G(1)\n"}, {"a.h", "#define G(y) F(y) #y\n#define A a\n"}},
      {{"a.h", "#define G(y) F(y)##y\n#define A b\n"}},
      {{"main.cpp", "#define F(...) <__VA_OPT__(x) __VA_ARGS__>\n#include \"a.h\"\nF() F(1) G(2)\n"}},
      {{"main.cpp", "#include \"g.h\"\n#include \"g.h\"\n#define G2\n#include \"g.h\"\nint m = __COUNTER__;\n"},
       {"g.h", "#ifndef G2\n#define G2\nint g = __COUNTER__;\n#endif\n"}},
      {{"g.h", "#ifndef G3\n#define G3\nint g = __COUNTER__ + __LINE__;\n#endif\n#if defined(G2) && __has_include(\"c.h\")\nint c;\n#endif\n"}},
      {{"c.h", "int c2;\n"}},
      {{"main.cpp", "#define G3\n#include \"g.h\"\n#undef G3\n#include \"g.h\"\n#line 7 \"n.cpp\"\nconst char* f = __FILE__;\n"}},
  };
  MemHost warm_host;
  pp::Session warm(warm_host);
  int step = 0;
  for (const auto& edit : states) {
    for (const auto& [path, text] : edit) warm_host.files[path] = text;
    warm.files.new_generation();
    auto inc = run(warm, warm_host);
    MemHost cold_host;
    cold_host.files = warm_host.files;
    pp::Session cold(cold_host);
    auto fresh = run(cold, cold_host);
    check(inc.region == fresh.region && inc.diags == fresh.diags, std::format("state {}: incremental equals cold", step));
    ++step;
  }
  check(warm.stats.reused > 0, "the warm session reused regions");
}

// A resource-only edit must invalidate a cached embed and a separately cached probe.
void embed_resources() {
  MemHost host;
  host.files["main.cpp"] = "#include \"data.h\"\n#include \"probe.h\"\n";
  host.files["data.h"] = "#embed \"data.bin\" if_empty(empty)\n";
  host.files["probe.h"] = R"cpp(
#if __has_embed("data.bin") == __STDC_EMBED_FOUND__
found
#elif __has_embed("data.bin") == __STDC_EMBED_EMPTY__
zero
#else
missing
#endif
)cpp";
  pp::Session warm(host);
  auto check_state = [&](std::string_view expected) {
    warm.files.new_generation();
    auto result = run(warm, host);
    pp::Session cold(host);
    auto fresh = run(cold, host);
    check(text(result.region) == expected, "resource bytes and availability reflect the current generation");
    check(result.region == fresh.region && result.diags == fresh.diags, "embed incremental equals cold");
    return result;
  };

  host.files["data.bin"] = std::string("\0\xff", 2);
  auto nonempty = check_state("0 , 255 found");
  const auto& r = nonempty.region;
  check(r.locs[0].expansion != 0 &&
            r.expansions[r.locs[0].expansion].kind == pp::ExpansionKind::embed &&
            r.files[r.expansions[r.locs[0].expansion].definition_file] == "data.bin" &&
            r.files[r.site(0).file] == "data.h",
        "the emitted byte refers to the directive and its resource");
  host.files["data.bin"] = "BC";
  check_state("66 , 67 found");
  host.files["data.bin"].clear();
  check_state("empty zero");
  host.files.erase("data.bin");
  check(kinds(check_state("missing")) == std::vector{lex::LexDiag::embed_not_found}, "a removed resource is diagnosed");
  host.files["data.bin"] = "D";
  check_state("68 found");
  pp::Config config;
  config.angle_dirs = {"."};
  pp::Session angles(host, config);
  auto macro_angle = pp::preprocess(angles, R"cpp(
#define PATH data.bin
#define PROBE __has_embed
#define OPEN (
#define CLOSE )
#if PROBE OPEN <PATH> CLOSE == __STDC_EMBED_FOUND__
yes
#endif
#define INDEX offset
#embed "data.bin" INDEX(defined(PATH)) if_empty(empty)
)cpp", "angle.cpp");
  check(macro_angle.diags.empty() && text(macro_angle.region) == "yes empty",
        "probe delimiters and an angle operand can come from macros, and offset evaluates defined");

  auto invalid = pp::preprocess(warm, R"cpp(
#define prefix suffix
#embed "data.bin" prefix(, 0)
)cpp", "invalid.cpp");
  check(kinds(invalid) == std::vector{lex::LexDiag::embed_parameter_macro},
        "a standard parameter name used as a macro is diagnosed before replacement");

  auto contextual = pp::preprocess(angles, R"cpp(
#if 0
#embed <unused/*name>
#endif
#embed "data.bin" limit(__has_include(<missing)header>)) if_empty(17)
)cpp", "contextual.cpp");
  check(contextual.diags.empty() && text(contextual.region) == "17",
        "contextual header names preserve lexer state and are opaque to operator delimiters");

  host.files["present.h"] = "";
  host.files["data.bin"] = std::string("\0\xff", 2);
  warm.files.new_generation();
  auto [printed, diagnostics] = pp::preprocess_to_text(warm, R"cpp(int data[] = {
#embed "data.bin" limit(1 + __has_include("present.h"))
};
)cpp", "print.cpp");
  check(diagnostics.empty() && printed == "# 1 \"print.cpp\"\nint data[] = {0, 255 };\n", "embed -E follows Clang's byte spacing");
}

void spacing() {
  using pp::detail::separate;
  check(separate("", "-", "-") && separate("", "<", ":") && separate("", "/", "*") && separate("", "x", "y"), "pastes");
  check(!separate("", "x", "-") && !separate("", "*", "/") && !separate("", ".", ".") && !separate("", "x", "\"s\""), "no paste");
  check(separate("", "1", "+") && separate(".", ".", ".") && !separate("", "1", "'2'"), "Clang's extra rules");
}

// `pp` on a pool (#137): units of one configuration, preprocessed by several workers at once
// through one process session, equal the same units preprocessed one at a time.
using u32 = std::uint32_t;
struct AllPp {
  using Key = u32;  // how many units
  using Value = std::vector<Fp128>;
  static engine::query_task<Value> run(engine::Ctx& cx, u32 n) {
    std::vector<pp::FileId> files;
    for (u32 i = 0; i < n; ++i) files.push_back(pp::FileId{i});
    const auto& regions = co_await cx.par_map<pp::Pp>(files);
    Value out;
    for (const auto& r : regions) out.push_back(fingerprint(r.get()));
    co_return out;
  }
};

void pool() {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / std::format("ycxx-pp-pool-{}", std::random_device{}());
  fs::create_directories(dir);
  auto write = [&](std::string_view name, std::string_view text) { std::ofstream(dir / name) << text; };
  write("shared.h", "#pragma once\n#define TWICE(x) ((x) + (x))\n#define SHARED TWICE(21)\nint shared_fn(int);\n");
  for (int k = 0; k < 3; ++k) write(std::format("own{}.h", k), std::format("#include \"shared.h\"\n#define OWN{} {}\n", k, k));
  constexpr u32 n = 48;
  auto preprocess_all = [&](engine::Db& db) {
    db.set_provider<pp::FileContent>(pp::read_disk);
    for (u32 i = 0; i < n; ++i) {
      db.set<pp::SourceOf>(pp::FileId{i}, {std::format("u{}.cpp", i), dir.native_encoded_string(), {}});
      db.set<pp::FileText>(pp::FileId{i}, std::format("#include \"own{0}.h\"\n#include \"shared.h\"\nint f{1}() {{ return SHARED + OWN{0} + {1}; }}\n", i % 3, i));
    }
    return db.get<AllPp>(n);
  };
  engine::Db sequential;
  const std::vector<Fp128> want = preprocess_all(sequential);
  for (int round = 0; round < 3; ++round) {
    engine::Pool workers(8);
    engine::Db parallel(workers);
    check(preprocess_all(parallel) == want, "pp on 8 workers equals pp on one");
  }
  fs::remove_all(dir);
}

}  // namespace

int main() {
  includes();
  linux_system_includes();
  linux_stddef();
  source_target();
  object_like();
  function_like();
  conditionals();
  traces();
  reuse();
  roots_and_depth();
  incremental_equals_cold();
  embed_resources();
  spacing();
  pool();
  return unit::result("unit.pp");
}
