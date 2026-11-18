// Phase 4 (ycxx.pp): include resolution, object-like replacement, the questions a region records,
// reuse of regions whose traces verify, and incremental equals cold over edit sequences
// (DECISIONS §4.2). `-E` output is compared with Clang's by the preprocess tests (tests/own/pp).
import std;
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

void spacing() {
  using pp::detail::separate;
  check(separate("", "-", "-") && separate("", "<", ":") && separate("", "/", "*") && separate("", "x", "y"), "pastes");
  check(!separate("", "x", "-") && !separate("", "*", "/") && !separate("", ".", ".") && !separate("", "x", "\"s\""), "no paste");
  check(separate("", "1", "+") && separate(".", ".", ".") && !separate("", "1", "'2'"), "Clang's extra rules");
}

}  // namespace

int main() {
  includes();
  object_like();
  traces();
  reuse();
  incremental_equals_cold();
  spacing();
  return unit::result("unit.pp");
}
