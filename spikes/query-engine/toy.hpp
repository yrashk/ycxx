// Throwaway spike for yrashk/ycxx#1: a toy language with C++-like overloading, compiled through
// the query engine in engine.hpp.
//
//   include "h.hdr";
//   fn f(int a) -> int;                                  // declaration
//   fn g(float x, int y) -> float { let t = f(y); return x + t; }
//
// Types are int, float and bool. Overload resolution ranks implicit conversions like
// [over.ics.rank]: exact match < promotion (bool -> int) < conversion (everything else), and picks
// the candidate that is no worse for every argument and better for at least one
// ([over.match.best]), or reports an ambiguity. Simplifications against C++: every declaration
// in the TU and its headers is visible everywhere (no point of declaration), and headers hold only
// declarations.
#pragma once

#include "engine.hpp"

#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace toy {

using qe::Ctx;
using qe::Key;
using qe::Value;

// --- syntax ---------------------------------------------------------------------------------

enum class Ty : std::uint8_t { Int, Float, Bool, Error };

inline const char* name(Ty t) {
  switch (t) {
    case Ty::Int: return "int";
    case Ty::Float: return "float";
    case Ty::Bool: return "bool";
    case Ty::Error: return "<error>";
  }
  return "?";
}

struct Signature {
  std::string name;
  std::vector<Ty> params;
  Ty ret = Ty::Int;
  friend bool operator==(const Signature&, const Signature&) = default;
  friend auto operator<=>(const Signature&, const Signature&) = default;
};

inline std::string types_key(const std::vector<Ty>& ts) {
  std::string s;
  for (std::size_t i = 0; i < ts.size(); ++i) s += (i ? "," : "") + std::string(name(ts[i]));
  return s;
}
inline std::string sig_key(const Signature& s) { return s.name + "(" + types_key(s.params) + ")"; }
inline std::string sig_text(const Signature& s) { return sig_key(s) + " -> " + name(s.ret); }

// Positions inside a body are relative to the item's first line, so edits above an item do not
// change its body's value (and so do not invalidate its checking).
struct Expr {
  enum Kind : std::uint8_t { IntLit, FloatLit, BoolLit, Var, Call, Add } kind = IntLit;
  std::string text;
  std::vector<Expr> args;
  int rel_line = 0;
  friend bool operator==(const Expr&, const Expr&) = default;
};

struct Stmt {
  bool is_return = false;
  std::string var;
  Expr e;
  int rel_line = 0;
  friend bool operator==(const Stmt&, const Stmt&) = default;
};

struct Diag {
  int line = 0;
  std::string msg;
  friend bool operator==(const Diag&, const Diag&) = default;
  friend auto operator<=>(const Diag&, const Diag&) = default;
};

struct Item {
  std::string id;  // stable identity: signature key, plus "#n" for the n-th duplicate in a file
  Signature sig;
  std::vector<std::string> pnames;
  bool has_body = false;
  std::vector<Stmt> body;
  int line = 0;
  friend bool operator==(const Item&, const Item&) = default;
};

struct FileAst {
  bool present = false;
  std::vector<std::string> includes;
  std::vector<Item> items;
  std::vector<Diag> diags;
  friend bool operator==(const FileAst&, const FileAst&) = default;
};

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}

  FileAst parse() {
    FileAst f;
    f.present = true;
    std::map<std::string, int> seen;
    while (skip(), p_ < s_.size()) {
      if (word("include")) {
        auto str = string_lit();
        expect(';');
        if (str) f.includes.push_back(*str);
        else return fail(f);
      } else if (word("fn")) {
        Item it;
        it.line = line_;
        it.sig.name = ident();
        if (it.sig.name.empty() || !expect('(')) return fail(f);
        if (!peek(')')) {
          do {
            auto t = type();
            auto n = ident();
            if (!t || n.empty()) return fail(f);
            it.sig.params.push_back(*t);
            it.pnames.push_back(n);
          } while (eat(','));
        }
        if (!expect(')') || !eat('-') || !expect('>')) return fail(f);
        auto r = type();
        if (!r) return fail(f);
        it.sig.ret = *r;
        if (eat('{')) {
          it.has_body = true;
          base_ = it.line;
          while (!eat('}')) {
            if (p_ >= s_.size()) return fail(f);
            Stmt st;
            st.rel_line = line_ - base_;
            if (word("return")) st.is_return = true;
            else if (word("let")) {
              st.var = ident();
              if (st.var.empty() || !expect('=')) return fail(f);
            } else return fail(f);
            auto e = expr();
            if (!e || !expect(';')) return fail(f);
            st.e = std::move(*e);
            it.body.push_back(std::move(st));
          }
        } else if (!expect(';')) return fail(f);
        int n = seen[sig_key(it.sig)]++;
        it.id = sig_key(it.sig) + (n ? "#" + std::to_string(n + 1) : "");
        f.items.push_back(std::move(it));
      } else return fail(f);
    }
    return f;
  }

 private:
  FileAst fail(FileAst f) {
    f.diags.push_back({line_, "syntax error"});
    return f;
  }
  void skip() {
    for (;;) {
      while (p_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[p_])))
        if (s_[p_++] == '\n') ++line_;
      if (s_.compare(p_, 2, "//") == 0) {
        while (p_ < s_.size() && s_[p_] != '\n') ++p_;
        continue;
      }
      return;
    }
  }
  bool peek(char c) {
    skip();
    return p_ < s_.size() && s_[p_] == c;
  }
  bool eat(char c) {
    if (!peek(c)) return false;
    ++p_;
    return true;
  }
  bool expect(char c) { return eat(c); }
  std::string ident() {
    skip();
    std::size_t b = p_;
    while (p_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[p_])) || s_[p_] == '_')) ++p_;
    if (b < s_.size() && std::isdigit(static_cast<unsigned char>(s_[b]))) {
      p_ = b;
      return {};
    }
    return s_.substr(b, p_ - b);
  }
  bool word(const char* w) {
    skip();
    std::size_t save = p_;
    if (ident() == w) return true;
    p_ = save;
    return false;
  }
  std::optional<std::string> string_lit() {
    if (!eat('"')) return std::nullopt;
    std::size_t b = p_;
    while (p_ < s_.size() && s_[p_] != '"') ++p_;
    if (p_ >= s_.size()) return std::nullopt;
    return s_.substr(b, p_++ - b);
  }
  std::optional<Ty> type() {
    if (word("int")) return Ty::Int;
    if (word("float")) return Ty::Float;
    if (word("bool")) return Ty::Bool;
    return std::nullopt;
  }
  std::optional<Expr> expr() {
    auto l = term();
    while (l && eat('+')) {
      auto r = term();
      if (!r) return std::nullopt;
      Expr a{Expr::Add, "", {std::move(*l), std::move(*r)}, line_ - base_};
      l = std::move(a);
    }
    return l;
  }
  std::optional<Expr> term() {
    skip();
    int rl = line_ - base_;
    if (eat('(')) {
      auto e = expr();
      if (!e || !expect(')')) return std::nullopt;
      return e;
    }
    if (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) {
      std::size_t b = p_;
      while (p_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.')) ++p_;
      auto t = s_.substr(b, p_ - b);
      return Expr{t.find('.') != std::string::npos ? Expr::FloatLit : Expr::IntLit, t, {}, rl};
    }
    if (word("true")) return Expr{Expr::BoolLit, "true", {}, rl};
    if (word("false")) return Expr{Expr::BoolLit, "false", {}, rl};
    auto id = ident();
    if (id.empty()) return std::nullopt;
    if (!eat('(')) return Expr{Expr::Var, id, {}, rl};
    Expr c{Expr::Call, id, {}, rl};
    if (!peek(')')) {
      do {
        auto a = expr();
        if (!a) return std::nullopt;
        c.args.push_back(std::move(*a));
      } while (eat(','));
    }
    if (!expect(')')) return std::nullopt;
    return c;
  }

  const std::string& s_;
  std::size_t p_ = 0;
  int line_ = 1;
  int base_ = 0;
};

// --- query results --------------------------------------------------------------------------

struct ItemHead {
  bool present = false;
  Signature sig;
  bool has_body = false;
  friend bool operator==(const ItemHead&, const ItemHead&) = default;
};

struct ItemBody {
  bool present = false;
  std::vector<std::string> pnames;
  std::vector<Stmt> body;
  friend bool operator==(const ItemBody&, const ItemBody&) = default;
};

struct Resolution {
  bool ok = false;
  Signature chosen;
  std::string error;
  friend bool operator==(const Resolution&, const Resolution&) = default;
};

struct Checked {
  std::vector<std::string> ir;
  std::vector<Diag> diags;  // lines relative to the item
  friend bool operator==(const Checked&, const Checked&) = default;
};

struct FullDiag {
  std::string file;
  int line = 0;
  std::string msg;
  friend bool operator==(const FullDiag&, const FullDiag&) = default;
  friend auto operator<=>(const FullDiag&, const FullDiag&) = default;
};

struct TuOutput {
  std::string code;
  std::vector<FullDiag> diags;
  friend bool operator==(const TuOutput&, const TuOutput&) = default;
};

using SourceText = std::optional<std::string>;  // nullopt: no such file

enum Kind : std::uint16_t {
  SOURCE = 1,   // input: file -> text
  TUS,          // input: list of translation units
  PARSE,        // file -> FileAst
  PARSE_DIAGS,  // file -> syntax diagnostics (projection)
  ITEMS,        // file -> item ids (projection)
  ITEM_HEAD,    // (file, item) -> signature (projection)
  ITEM_BODY,    // (file, item) -> body with relative positions (projection)
  ITEM_LINE,    // (file, item) -> first line (projection)
  FILE_INDEX,   // file -> name -> signatures declared in the file
  DECLS_NAMED,  // (file, name) -> signatures named `name` declared in the file
  VISIBLE,      // tu -> files visible in it, in include order
  OVERLOADS,    // (tu, name) -> overload set
  RESOLVE,      // (tu, "name|argtypes") -> chosen overload or error
  CHECK,        // (file, item) -> typed body + diagnostics
  LOWER,        // (file, item) -> "object code"
  FN_LIST,      // tu -> items with bodies
  TU_OUTPUT,    // tu -> code + diagnostics
  OUTPUT,       // () -> whole program
  KIND_COUNT
};

inline const char* kind_name(std::uint16_t k) {
  static const char* names[] = {"?",         "SOURCE",    "TUS",       "PARSE",       "PARSE_DIAGS",
                                "ITEMS",     "ITEM_HEAD", "ITEM_BODY", "ITEM_LINE",   "FILE_INDEX",
                                "DECLS_NAMED", "VISIBLE", "OVERLOADS", "RESOLVE",     "CHECK",
                                "LOWER",     "FN_LIST",   "TU_OUTPUT", "OUTPUT"};
  return k < KIND_COUNT ? names[k] : "?";
}

// Queries that model real work; the rest are cheap projections.
inline bool expensive(std::uint16_t k) { return k == RESOLVE || k == CHECK || k == LOWER; }

struct Options {
  int work = 0;               // simulated work per CHECK/LOWER, to make parallelism visible
  bool untracked_bug = false;  // fault injection: RESOLVE reads OVERLOADS without recording it
};

inline void burn(int n, const std::string& seed) {
  volatile std::uint64_t h = 0;
  std::uint64_t x = std::hash<std::string>{}(seed);
  for (int i = 0; i < n; ++i) x = x * 6364136223846793005ull + 1442695040888963407ull;
  h = x;
  (void)h;
}

// --- queries --------------------------------------------------------------------------------

inline Key key(Kind k, std::string a = {}, std::string b = {}) { return Key{k, std::move(a), std::move(b)}; }

inline const Item* find_item(const FileAst& f, const std::string& id) {
  for (auto& it : f.items)
    if (it.id == id) return &it;
  return nullptr;
}

inline void define_queries(qe::Engine& e, const Options& opt) {
  using qe::box;
  using qe::unbox;

  // A source file nobody set is a missing file, not an error in the engine.
  e.define(SOURCE, [](Ctx&, const Key&) { return box(SourceText{}); });
  e.define(TUS, [](Ctx&, const Key&) { return box(std::vector<std::string>{}); });

  e.define(PARSE, [](Ctx& c, const Key& k) {
    auto& src = unbox<SourceText>(c.fetch(key(SOURCE, k.a)));
    if (!src) return box(FileAst{});
    return box(Parser(*src).parse());
  });

  e.define(PARSE_DIAGS, [](Ctx& c, const Key& k) {
    auto& f = unbox<FileAst>(c.fetch(key(PARSE, k.a)));
    auto d = f.diags;
    if (!f.present) d.push_back({0, "file not found"});
    return box(d);
  });

  e.define(ITEMS, [](Ctx& c, const Key& k) {
    std::vector<std::string> ids;
    for (auto& it : unbox<FileAst>(c.fetch(key(PARSE, k.a))).items) ids.push_back(it.id);
    return box(ids);
  });

  // Projections are total: validation may ask about an item that no longer exists.
  e.define(ITEM_HEAD, [](Ctx& c, const Key& k) {
    auto* it = find_item(unbox<FileAst>(c.fetch(key(PARSE, k.a))), k.b);
    return box(it ? ItemHead{true, it->sig, it->has_body} : ItemHead{});
  });
  e.define(ITEM_BODY, [](Ctx& c, const Key& k) {
    auto* it = find_item(unbox<FileAst>(c.fetch(key(PARSE, k.a))), k.b);
    return box(it ? ItemBody{true, it->pnames, it->body} : ItemBody{});
  });
  e.define(ITEM_LINE, [](Ctx& c, const Key& k) {
    auto* it = find_item(unbox<FileAst>(c.fetch(key(PARSE, k.a))), k.b);
    return box(it ? it->line : -1);
  });

  e.define(FILE_INDEX, [](Ctx& c, const Key& k) {
    std::map<std::string, std::vector<Signature>> idx;
    for (auto& it : unbox<FileAst>(c.fetch(key(PARSE, k.a))).items) {
      auto& v = idx[it.sig.name];
      if (std::find(v.begin(), v.end(), it.sig) == v.end()) v.push_back(it.sig);
    }
    for (auto& [n, v] : idx) std::sort(v.begin(), v.end());
    return box(idx);
  });

  // The per-(file, name) firewall: a new declaration of `h` changes DECLS_NAMED(file, h) only,
  // and an empty result (nothing named `k` here) is recorded like any other.
  e.define(DECLS_NAMED, [](Ctx& c, const Key& k) {
    auto& idx = unbox<std::map<std::string, std::vector<Signature>>>(c.fetch(key(FILE_INDEX, k.a)));
    auto i = idx.find(k.b);
    return box(i == idx.end() ? std::vector<Signature>{} : i->second);
  });

  e.define(VISIBLE, [](Ctx& c, const Key& k) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    // Depth-first in include order; a file is entered once (as if every header had a guard).
    std::function<void(const std::string&)> visit = [&](const std::string& f) {
      if (!seen.insert(f).second) return;
      out.push_back(f);
      for (auto& inc : unbox<FileAst>(c.fetch(key(PARSE, f))).includes) visit(inc);
    };
    visit(k.a);
    return box(out);
  });

  e.define(OVERLOADS, [](Ctx& c, const Key& k) {
    auto& files = unbox<std::vector<std::string>>(c.fetch(key(VISIBLE, k.a)));
    std::vector<Key> ks;
    for (auto& f : files) ks.push_back(key(DECLS_NAMED, f, k.b));
    std::vector<Signature> set;
    for (auto& v : c.fetch_all(ks))
      for (auto& s : unbox<std::vector<Signature>>(v))
        if (std::find_if(set.begin(), set.end(), [&](auto& x) { return x.params == s.params; }) == set.end())
          set.push_back(s);
    std::sort(set.begin(), set.end());
    return box(set);
  });

  e.define(RESOLVE, [&opt](Ctx& c, const Key& k) {
    auto bar = k.b.find('|');
    std::string fname = k.b.substr(0, bar);
    std::vector<Ty> args;
    for (std::size_t p = bar + 1; p < k.b.size();) {
      auto q = k.b.find(',', p);
      if (q == std::string::npos) q = k.b.size();
      auto t = k.b.substr(p, q - p);
      args.push_back(t == "int" ? Ty::Int : t == "float" ? Ty::Float : Ty::Bool);
      p = q + 1;
    }
    auto ov = opt.untracked_bug ? c.fetch_untracked(key(OVERLOADS, k.a, fname)) : c.fetch(key(OVERLOADS, k.a, fname));
    auto& set = unbox<std::vector<Signature>>(ov);
    std::string call = fname + "(" + types_key(args) + ")";
    if (set.empty()) return box(Resolution{false, {}, "no function named '" + fname + "'"});
    // [over.ics.rank], simplified: 0 exact, 1 promotion, 2 conversion.
    auto rank = [](Ty from, Ty to) { return from == to ? 0 : (from == Ty::Bool && to == Ty::Int) ? 1 : 2; };
    std::vector<std::vector<int>> ranks;
    std::vector<const Signature*> viable;
    for (auto& s : set)
      if (s.params.size() == args.size()) {
        std::vector<int> r;
        for (std::size_t i = 0; i < args.size(); ++i) r.push_back(rank(args[i], s.params[i]));
        viable.push_back(&s);
        ranks.push_back(std::move(r));
      }
    if (viable.empty()) return box(Resolution{false, {}, "no matching function for call to '" + call + "'"});
    auto better = [&](std::size_t a, std::size_t b) {  // [over.match.best]
      bool strictly = false;
      for (std::size_t i = 0; i < args.size(); ++i) {
        if (ranks[a][i] > ranks[b][i]) return false;
        if (ranks[a][i] < ranks[b][i]) strictly = true;
      }
      return strictly;
    };
    std::size_t best = 0;
    for (std::size_t i = 1; i < viable.size(); ++i)
      if (better(i, best)) best = i;
    for (std::size_t i = 0; i < viable.size(); ++i)
      if (i != best && !better(best, i)) return box(Resolution{false, {}, "call to '" + call + "' is ambiguous"});
    return box(Resolution{true, *viable[best], {}});
  });

  e.define(CHECK, [&opt](Ctx& c, const Key& k) {
    auto& head = unbox<ItemHead>(c.fetch(key(ITEM_HEAD, k.a, k.b)));
    auto& body = unbox<ItemBody>(c.fetch(key(ITEM_BODY, k.a, k.b)));
    Checked out;
    if (!head.present || !body.present) return box(out);
    burn(opt.work, k.b);
    std::map<std::string, Ty> env;
    for (std::size_t i = 0; i < body.pnames.size(); ++i) env[body.pnames[i]] = head.sig.params[i];
    int tmp = 0;
    std::function<std::pair<Ty, std::string>(const Expr&)> type = [&](const Expr& e) -> std::pair<Ty, std::string> {
      switch (e.kind) {
        case Expr::IntLit: return {Ty::Int, e.text};
        case Expr::FloatLit: return {Ty::Float, e.text};
        case Expr::BoolLit: return {Ty::Bool, e.text};
        case Expr::Var: {
          auto i = env.find(e.text);
          if (i == env.end()) {
            out.diags.push_back({e.rel_line, "use of undeclared identifier '" + e.text + "'"});
            return {Ty::Error, "?"};
          }
          return {i->second, e.text};
        }
        case Expr::Add: {
          auto [l, ln] = type(e.args[0]);
          auto [r, rn] = type(e.args[1]);
          if (l == Ty::Error || r == Ty::Error) return {Ty::Error, "?"};
          Ty t = (l == Ty::Float || r == Ty::Float) ? Ty::Float : Ty::Int;
          auto v = "%" + std::to_string(tmp++);
          out.ir.push_back(v + " = add." + name(t) + " " + ln + ", " + rn);
          return {t, v};
        }
        case Expr::Call: {
          std::vector<Ty> ats;
          std::string as;
          bool bad = false;
          for (auto& a : e.args) {
            auto [t, n] = type(a);
            bad |= t == Ty::Error;
            ats.push_back(t);
            as += (as.empty() ? "" : ", ") + n;
          }
          if (bad) return {Ty::Error, "?"};
          auto& r = unbox<Resolution>(c.fetch(key(RESOLVE, k.a, e.text + "|" + types_key(ats))));
          if (!r.ok) {
            out.diags.push_back({e.rel_line, r.error});
            return {Ty::Error, "?"};
          }
          auto v = "%" + std::to_string(tmp++);
          out.ir.push_back(v + " = call " + sig_text(r.chosen) + " (" + as + ")");
          return {r.chosen.ret, v};
        }
      }
      return {Ty::Error, "?"};
    };
    bool returned = false;
    for (auto& st : body.body) {
      auto [t, v] = type(st.e);
      if (st.is_return) {
        out.ir.push_back("ret." + std::string(name(head.sig.ret)) + " " + v);
        returned = true;
      } else env[st.var] = t;
    }
    if (!returned) out.diags.push_back({0, "function '" + head.sig.name + "' does not return a value"});
    return box(out);
  });

  e.define(LOWER, [&opt](Ctx& c, const Key& k) {
    auto& ch = unbox<Checked>(c.fetch(key(CHECK, k.a, k.b)));
    burn(opt.work, k.b);
    std::string s = k.b + ":\n";
    for (auto& l : ch.ir) s += "  " + l + "\n";
    return box(s);
  });

  e.define(FN_LIST, [](Ctx& c, const Key& k) {
    auto& ids = unbox<std::vector<std::string>>(c.fetch(key(ITEMS, k.a)));
    std::vector<Key> ks;
    for (auto& id : ids) ks.push_back(key(ITEM_HEAD, k.a, id));
    std::vector<std::string> out;
    auto heads = c.fetch_all(ks);
    for (std::size_t i = 0; i < ids.size(); ++i)
      if (unbox<ItemHead>(heads[i]).has_body) out.push_back(ids[i]);
    return box(out);
  });

  e.define(TU_OUTPUT, [](Ctx& c, const Key& k) {
    auto& fns = unbox<std::vector<std::string>>(c.fetch(key(FN_LIST, k.a)));
    std::vector<Key> lower, check, line;
    for (auto& f : fns) {
      lower.push_back(key(LOWER, k.a, f));
      check.push_back(key(CHECK, k.a, f));
      line.push_back(key(ITEM_LINE, k.a, f));
    }
    TuOutput out;
    // Fan out in parallel; concatenate in item order, never completion order.
    auto lv = c.fetch_all(lower);
    auto cv = c.fetch_all(check);
    auto nv = c.fetch_all(line);
    for (std::size_t i = 0; i < fns.size(); ++i) {
      out.code += unbox<std::string>(lv[i]);
      for (auto& d : unbox<Checked>(cv[i]).diags) out.diags.push_back({k.a, unbox<int>(nv[i]) + d.line, d.msg});
    }
    // Syntax errors of every visible file; a header's appear once per TU and are deduplicated
    // program-wide in OUTPUT.
    auto& files = unbox<std::vector<std::string>>(c.fetch(key(VISIBLE, k.a)));
    std::vector<Key> pd;
    for (auto& f : files) pd.push_back(key(PARSE_DIAGS, f));
    auto pv = c.fetch_all(pd);
    for (std::size_t i = 0; i < files.size(); ++i)
      for (auto& d : unbox<std::vector<Diag>>(pv[i])) out.diags.push_back({files[i], d.line, d.msg});
    return box(out);
  });

  e.define(OUTPUT, [](Ctx& c, const Key&) {
    auto tus = unbox<std::vector<std::string>>(c.fetch(key(TUS)));
    std::sort(tus.begin(), tus.end());
    std::vector<Key> ks;
    for (auto& t : tus) ks.push_back(key(TU_OUTPUT, t));
    auto vs = c.fetch_all(ks);
    std::string s;
    std::set<FullDiag> diags;  // ordered by (file, line, message), duplicates merged
    for (std::size_t i = 0; i < tus.size(); ++i) {
      auto& o = unbox<TuOutput>(vs[i]);
      s += "== " + tus[i] + "\n" + o.code;
      diags.insert(o.diags.begin(), o.diags.end());
    }
    s += "== diagnostics\n";
    for (auto& d : diags) s += d.file + ":" + std::to_string(d.line) + ": error: " + d.msg + "\n";
    return box(s);
  });
}

}  // namespace toy
