// Throwaway spike for yrashk/ycxx#1. Demonstrates, on the toy language in toy.hpp:
//   (a) editing one function body recomputes only that function;
//   (b) adding an overload in a header invalidates exactly the affected call sites;
//   (c) identical results (outputs, executed queries, the whole memo graph) at 1 and N threads;
// plus the incremental == cold invariant over random edit sequences, and that the same checker
// catches a deliberately missed dependency.
//
//   spike demo            (a) and (b), with the executed-query logs
//   spike threads N EDITS (c): builds at 1..N threads, cold then after each edit, compared
//   spike fuzz STEPS N    random edits; incremental at 1 and N threads vs cold, every step
//   spike fuzz-bug STEPS  the same with an untracked read injected; must fail

#include "toy.hpp"

#include <chrono>
#include <cstdlib>
#include <random>

using namespace toy;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

struct World {
  std::map<std::string, std::string> files;
  std::set<std::string> deleted;  // files that existed once; their input becomes "no such file"
  std::vector<std::string> tus;
};

void load(qe::Engine& e, const World& w) {
  for (auto& [f, text] : w.files) e.set_input(key(SOURCE, f), qe::box(SourceText{text}));
  for (auto& f : w.deleted) e.set_input(key(SOURCE, f), qe::box(SourceText{}));
  e.set_input(key(TUS), qe::box(w.tus));
}

std::string build(qe::Engine& e) { return qe::unbox<std::string>(e.get(key(OUTPUT))); }

void print_log(const std::vector<qe::LogEntry>& log) {
  std::printf("  executed %zu queries:\n", log.size());
  for (auto& l : log)
    std::printf("    %-11s %-24s %-22s %s\n", kind_name(l.key.kind), l.key.a.c_str(), l.key.b.c_str(),
                l.changed ? "changed" : "same (cutoff)");
}

std::set<std::string> executed(const std::vector<qe::LogEntry>& log, Kind k) {
  std::set<std::string> s;
  for (auto& l : log)
    if (l.key.kind == k) s.insert(l.key.a + " " + l.key.b);
  return s;
}

std::set<std::string> changed(const std::vector<qe::LogEntry>& log, Kind k) {
  std::set<std::string> s;
  for (auto& l : log)
    if (l.key.kind == k && l.changed) s.insert(l.key.a + " " + l.key.b);
  return s;
}

std::string with(const std::string& s, const std::string& from, const std::string& to) {
  auto p = s.find(from);
  if (p == std::string::npos) std::abort();
  return s.substr(0, p) + to + s.substr(p + from.size());
}

// ---------------------------------------------------------------------------------------------

int demo() {
  World w;
  w.files["h.hdr"] =
      "fn f(int a) -> int;\n"
      "fn g(float a) -> float;\n";
  w.files["a.src"] =
      "include \"h.hdr\";\n"
      "fn use_int(int x) -> int { return f(x) + 1; }\n"
      "fn use_float(float y) -> int { return f(y); }\n"
      "fn use_g(int z) -> float { return g(z); }\n"
      "fn local(int q) -> int {\n"
      "  let r = q + 2;\n"
      "  return r;\n"
      "}\n"
      "fn calls_missing(int x) -> int { return k(x); }\n";
  w.files["b.src"] =
      "include \"h.hdr\";\n"
      "fn b1(bool t) -> int { return f(t); }\n"
      "fn b2(int t) -> float { return g(t); }\n";
  w.tus = {"a.src", "b.src"};

  Options opt;
  qe::Engine e(4);
  define_queries(e, opt);
  load(e, w);
  std::printf("cold build:\n%s\n", build(e).c_str());
  e.take_log();

  std::printf("(a) edit the body of `local` in a.src (and grow it by a line)\n");
  w.files["a.src"] = with(w.files["a.src"], "  let r = q + 2;\n", "  let r = q + 3;\n  let s = r + r;\n");
  w.files["a.src"] = with(w.files["a.src"], "  return r;\n", "  return s;\n");
  load(e, w);
  build(e);
  auto log = e.take_log();
  print_log(log);
  check(executed(log, CHECK) == std::set<std::string>{"a.src local(int)"}, "only CHECK(local) re-executed");
  check(executed(log, LOWER) == std::set<std::string>{"a.src local(int)"}, "only LOWER(local) re-executed");
  check(executed(log, RESOLVE).empty(), "no overload resolution re-executed");
  check(executed(log, TU_OUTPUT) == std::set<std::string>{"a.src "}, "b.src's TU output untouched");

  std::printf("\n(a') add two lines above everything in a.src: positions shift, nothing is re-checked\n");
  w.files["a.src"] = "// a comment\n\n" + w.files["a.src"];
  load(e, w);
  auto out = build(e);
  log = e.take_log();
  print_log(log);
  check(executed(log, CHECK).empty() && executed(log, LOWER).empty(), "no CHECK/LOWER re-executed");
  check(out.find("a.src:12: error: no function named 'k'") != std::string::npos, "diagnostic line moved with the code");

  std::printf("\n(b) add `fn f(float a) -> float;` to h.hdr\n");
  w.files["h.hdr"] += "fn f(float a) -> float;\n";
  load(e, w);
  out = build(e);
  log = e.take_log();
  print_log(log);
  check(executed(log, RESOLVE) ==
            std::set<std::string>{"a.src f|float", "a.src f|int", "b.src f|bool"},
        "exactly the resolutions of calls naming f were re-run");
  check(changed(log, RESOLVE) == std::set<std::string>{"a.src f|float"}, "only f(float-arg) changed its winner");
  check(executed(log, CHECK) == std::set<std::string>{"a.src use_float(float)"}, "only use_float re-checked");
  check(executed(log, DECLS_NAMED) ==
            std::set<std::string>{"h.hdr f", "h.hdr g", "h.hdr k"},
        "per-name firewalls re-ran, including the negative one for k");
  check(changed(log, DECLS_NAMED) == std::set<std::string>{"h.hdr f"}, "only DECLS_NAMED(h.hdr, f) changed");

  std::printf("\n(b') add `fn k(int a) -> int;` to h.hdr: a negative lookup result flips\n");
  w.files["h.hdr"] += "fn k(int a) -> int;\n";
  load(e, w);
  out = build(e);
  log = e.take_log();
  print_log(log);
  check(executed(log, CHECK) == std::set<std::string>{"a.src calls_missing(int)"}, "only calls_missing re-checked");
  check(out.find("no function named 'k'") == std::string::npos, "the error is gone");

  std::printf("\n(b'') add an unrelated `fn unused(bool a) -> int;` to h.hdr\n");
  w.files["h.hdr"] += "fn unused(bool a) -> int;\n";
  load(e, w);
  build(e);
  log = e.take_log();
  print_log(log);
  check(executed(log, OVERLOADS).empty() && executed(log, RESOLVE).empty() && executed(log, CHECK).empty(),
        "no overload set, resolution or body re-executed");

  std::printf("\n(b''') add `fn g(int a) -> int;` to h.hdr: use_g and b2 now pick it, in both TUs\n");
  w.files["h.hdr"] += "fn g(int a) -> int;\n";
  load(e, w);
  out = build(e);
  log = e.take_log();
  print_log(log);
  check(executed(log, CHECK) == std::set<std::string>{"a.src use_g(int)", "b.src b2(int)"}, "use_g and b2 re-checked");
  std::printf("\nfinal build:\n%s\n", out.c_str());

  // Incremental equals cold.
  qe::Engine cold(4);
  define_queries(cold, opt);
  load(cold, w);
  check(build(cold) == out, "incremental result equals a cold build of the final state");
  return failures;
}

// ---------------------------------------------------------------------------------------------
// Random programs and edits.

struct GenFn {
  std::string name;
  std::vector<Ty> params;
  Ty ret;
  std::vector<std::string> stmts;
};
struct GenSource {
  std::vector<int> includes;
  std::vector<GenFn> fns;
  int leading = 0;  // leading comment lines (edits shift positions)
};
struct Program {
  std::vector<std::vector<std::string>> headers;  // declaration lines
  std::vector<bool> header_deleted;
  std::vector<GenSource> sources;
  int next_fn = 0;
};

struct Gen {
  std::mt19937_64 rng;
  int names = 30;
  explicit Gen(std::uint64_t seed) : rng(seed) {}
  int pick(int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); }
  Ty ty() { return static_cast<Ty>(pick(3)); }

  std::string decl() {
    std::string s = "fn n" + std::to_string(pick(names)) + "(";
    int np = 1 + pick(2);
    for (int i = 0; i < np; ++i) s += (i ? ", " : "") + std::string(name(ty())) + " p" + std::to_string(i);
    return s + ") -> " + name(ty()) + ";";
  }
  std::string arg(const GenFn& f) {
    switch (pick(4)) {
      case 0: return std::to_string(pick(9));
      case 1: return std::to_string(pick(9)) + ".5";
      case 2: return pick(2) ? "true" : "false";
      default: return f.params.empty() ? "1" : "p" + std::to_string(pick(static_cast<int>(f.params.size())));
    }
  }
  std::string stmt(const GenFn& f, int i) {
    std::string s = "let v" + std::to_string(i) + " = n" + std::to_string(pick(names + 3)) + "(";
    int na = 1 + pick(2);
    for (int a = 0; a < na; ++a) s += (a ? ", " : "") + arg(f);
    return s + ");";
  }
  GenFn fn(Program& p) {
    GenFn f{"s" + std::to_string(p.next_fn++), {}, ty(), {}};
    int np = 1 + pick(2);
    for (int i = 0; i < np; ++i) f.params.push_back(ty());
    int ns = 1 + pick(3);
    for (int i = 0; i < ns; ++i) f.stmts.push_back(stmt(f, i));
    return f;
  }
  Program program(int headers, int sources, int fns_per_source, int decls_per_header) {
    Program p;
    p.headers.resize(headers);
    p.header_deleted.resize(headers);
    for (auto& h : p.headers)
      for (int i = 0; i < decls_per_header; ++i) h.push_back(decl());
    p.sources.resize(sources);
    for (auto& s : p.sources) {
      for (int h = 0; h < headers; ++h)
        if (pick(2)) s.includes.push_back(h);
      for (int i = 0; i < fns_per_source; ++i) s.fns.push_back(fn(p));
    }
    return p;
  }

  // One random edit; returns a description.
  std::string edit(Program& p) {
    auto& s = p.sources[pick(static_cast<int>(p.sources.size()))];
    auto& h = p.headers[pick(static_cast<int>(p.headers.size()))];
    switch (pick(8)) {
      case 7: {
        int hi = pick(static_cast<int>(p.headers.size()));
        p.header_deleted[hi] = !p.header_deleted[hi];
        return (p.header_deleted[hi] ? "delete h" : "restore h") + std::to_string(hi) + ".hdr";
      }
      case 0:
      case 1: {
        if (s.fns.empty()) return "noop";
        auto& f = s.fns[pick(static_cast<int>(s.fns.size()))];
        int i = pick(static_cast<int>(f.stmts.size()));
        f.stmts[i] = stmt(f, i);
        return "edit body of " + f.name;
      }
      case 2: h.push_back(decl()); return "add declaration " + h.back();
      case 3: {
        if (h.empty()) return "noop";
        int i = pick(static_cast<int>(h.size()));
        auto d = h[i];
        h.erase(h.begin() + i);
        return "remove declaration " + d;
      }
      case 4: s.leading += 1 + pick(2); return "shift positions";
      case 5:
        if (pick(2) || s.fns.empty()) {
          s.fns.insert(s.fns.begin() + pick(static_cast<int>(s.fns.size()) + 1), fn(p));
          return "add function";
        } else {
          s.fns.erase(s.fns.begin() + pick(static_cast<int>(s.fns.size())));
          return "remove function";
        }
      default: {
        int hi = pick(static_cast<int>(p.headers.size()));
        auto it = std::find(s.includes.begin(), s.includes.end(), hi);
        if (it == s.includes.end()) {
          s.includes.push_back(hi);
          return "add include";
        }
        s.includes.erase(it);
        return "remove include";
      }
    }
  }
};

World render(const Program& p) {
  World w;
  for (std::size_t i = 0; i < p.headers.size(); ++i) {
    std::string t;
    for (auto& d : p.headers[i]) t += d + "\n";
    auto n = "h" + std::to_string(i) + ".hdr";
    if (p.header_deleted[i]) w.deleted.insert(n);
    else w.files[n] = t;
  }
  for (std::size_t i = 0; i < p.sources.size(); ++i) {
    auto& s = p.sources[i];
    std::string t;
    for (int l = 0; l < s.leading; ++l) t += "// shift\n";
    for (int h : s.includes) t += "include \"h" + std::to_string(h) + ".hdr\";\n";
    for (auto& f : s.fns) {
      t += "fn " + f.name + "(";
      for (std::size_t a = 0; a < f.params.size(); ++a)
        t += (a ? ", " : "") + std::string(name(f.params[a])) + " p" + std::to_string(a);
      t += ") -> " + std::string(name(f.ret)) + " {\n";
      for (auto& st : f.stmts) t += "  " + st + "\n";
      t += "  return v0;\n}\n";
    }
    auto n = "s" + std::to_string(i) + ".src";
    w.files[n] = t;
    w.tus.push_back(n);
  }
  return w;
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// A cold build can only differ across thread counts in what runs and in dependency order (every
// entry is new, so changed_at and cutoffs are trivially equal). The incremental builds that
// follow are where validation and cutoff could depend on scheduling, so they are compared too.
int threads_demo(unsigned max_threads, int edits) {
  struct Run {
    std::vector<std::string> outs;
    std::vector<std::vector<qe::LogEntry>> logs;
    std::vector<std::string> graphs;
  };
  Options opt;
  opt.work = 200000;
  Run ref;
  for (unsigned n = 1; n <= max_threads; n *= 2) {
    Gen g(42);
    Program p = g.program(8, 40, 50, 20);
    World w = render(p);
    qe::Engine e(n);
    define_queries(e, opt);
    load(e, w);
    auto t0 = std::chrono::steady_clock::now();
    Run r;
    r.outs.push_back(build(e));
    double dt = seconds_since(t0);
    r.logs.push_back(e.take_log());
    r.graphs.push_back(e.graph_digest());
    std::size_t reexecuted = 0;
    for (int i = 0; i < edits; ++i) {
      g.edit(p);
      load(e, render(p));
      r.outs.push_back(build(e));
      r.logs.push_back(e.take_log());
      r.graphs.push_back(e.graph_digest());
      reexecuted += r.logs.back().size();
    }
    std::printf("threads %2u: cold build %.3fs, %zu queries; then %d edits re-executing %zu queries\n", n, dt,
                r.logs[0].size(), edits, reexecuted);
    if (n == 1) {
      ref = std::move(r);
      continue;
    }
    check(r.outs[0] == ref.outs[0] && r.logs[0] == ref.logs[0] && r.graphs[0] == ref.graphs[0],
          "cold: same output, same queries executed, same dependency lists");
    check(r.outs == ref.outs, "after each edit: output identical to 1 thread");
    check(r.logs == ref.logs, "after each edit: same queries re-executed, same cutoffs");
    check(r.graphs == ref.graphs, "after each edit: memo graph (revisions, dependencies, execution counts) identical");
  }
  return failures;
}

// Exit codes: 0 ok, 3 incremental differs from cold, 4 the 1- and N-thread engines disagree.
int fuzz(int steps, unsigned threads, bool inject_bug) {
  Gen g(7);
  Program p = g.program(4, 12, 12, 12);
  Options opt;
  opt.untracked_bug = inject_bug;
  qe::Engine one(1), many(threads);
  define_queries(one, opt);
  define_queries(many, opt);
  World w = render(p);
  load(one, w);
  load(many, w);
  build(one), build(many);
  one.take_log(), many.take_log();
  for (int step = 1; step <= steps; ++step) {
    auto what = g.edit(p);
    w = render(p);
    load(one, w);
    load(many, w);
    auto a = build(one), b = build(many);
    auto la = one.take_log(), lb = many.take_log();
    qe::Engine cold(threads);
    define_queries(cold, opt);
    load(cold, w);
    auto c = build(cold);
    if (a != c || b != c) {
      std::printf("step %d (%s): %s incremental differs from cold\n", step, what.c_str(), a != c ? "1-thread" : "N-thread");
      return 3;
    }
    if (la != lb || (step % 10 == 0 && one.graph_digest() != many.graph_digest())) {
      std::printf("step %d (%s): 1 and N threads disagree\n", step, what.c_str());
      return 4;
    }
    if (step % 50 == 0) std::printf("step %d ok (%zu queries re-executed by the last edit: %s)\n", step, la.size(), what.c_str());
  }
  std::printf("%d edits: incremental (1 and %u threads) == cold after every edit\n", steps, threads);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = argc > 1 ? argv[1] : "demo";
  if (mode == "demo") return demo() ? 1 : 0;
  auto arg = [&](int i, int def) { return std::max(1, argc > i ? std::atoi(argv[i]) : def); };
  if (mode == "threads") return threads_demo(arg(2, 16), arg(3, 10)) ? 1 : 0;
  if (mode == "fuzz") return fuzz(arg(2, 300), arg(3, 8), false);
  if (mode == "fuzz-bug") {
    int r = fuzz(arg(2, 300), arg(3, 8), true);
    std::printf(r == 3 ? "the missed dependency was caught, as it should be\n" : "MISSED: the injected bug was not caught as incremental != cold\n");
    return r == 3 ? 0 : 1;
  }
  std::fprintf(stderr, "usage: spike [demo | threads N EDITS | fuzz STEPS N | fuzz-bug STEPS N]\n");
  return 2;
}
