// Demonstrations for the e-graph spike. See README.md.
#include "extract.hpp"

#include <cstring>
#include <string_view>

using namespace eg;
using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

static std::uint64_t splitmix(std::uint64_t& s) {
  std::uint64_t z = (s += 0x9e3779b97f4a7c15);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
  return z ^ (z >> 31);
}

// An input expression DAG: kids index earlier entries; the last entry is the root.
struct Expr { std::vector<Node> n; };
static Id load(EGraph& g, const Expr& e) {
  std::vector<Id> id;
  for (Node n : e.n) {
    for (int i = 0; i < arity(n.op); ++i) n.k[i] = id[n.k[i]];
    id.push_back(g.add(n));
  }
  return id.back();
}
static std::int64_t eval(const Expr& e, const std::vector<std::int64_t>& env) {
  std::vector<std::int64_t> v;
  for (const Node& n : e.n)
    v.push_back(n.op == Op::Var ? env[n.v]
                                : eval_op(n.op, n.v, arity(n.op) > 0 ? v[n.k[0]] : 0, arity(n.op) > 1 ? v[n.k[1]] : 0));
  return v.back();
}

struct B {  // tiny builder for hand-written examples
  Expr e;
  Id push(Node n) { e.n.push_back(n); return Id(e.n.size() - 1); }
  Id x(int i) { return push(var(i)); }
  Id c(std::int64_t v) { return push(cnst(v)); }
  Id op(Op o, Id a, Id b) { return push(bin(o, a, b)); }
};

static Expr random_expr(std::uint64_t seed, int size, int vars) {
  Expr e;
  std::uint64_t s = seed;
  for (int i = 0; i < vars; ++i) e.n.push_back(var(i));
  std::vector<int> uses;
  auto pick = [&] {
    int sz = int(e.n.size());
    int j = splitmix(s) % 2 ? sz - 1 - int(splitmix(s) % std::min(sz, 6)) : int(splitmix(s) % sz);
    return Id(j);
  };
  while (int(e.n.size()) < size) {
    auto r = splitmix(s) % 100;
    if (r < 15) e.n.push_back(cnst(std::int64_t(1 + splitmix(s) % 12)));
    else if (r < 20) e.n.push_back(un(Op::Neg, pick()));
    else if (r < 25) { Id a = pick(); e.n.push_back(cnst(std::int64_t(1 + splitmix(s) % 4))); e.n.push_back(bin(Op::Shl, a, Id(e.n.size() - 1))); }
    else {
      Op o = r < 55 ? Op::Add : r < 70 ? Op::Sub : Op::Mul;
      Id a = pick(), b = pick();
      if (o == Op::Mul && splitmix(s) % 2) { e.n.push_back(cnst(std::int64_t(2 + splitmix(s) % 11))); b = Id(e.n.size() - 1); }
      e.n.push_back(bin(o, a, b));
    }
  }
  // Root: the sum of every entry nobody uses.
  std::vector<char> used(e.n.size());
  for (const Node& n : e.n) for (int i = 0; i < arity(n.op); ++i) used[n.k[i]] = 1;
  std::optional<Id> root;
  for (Id i = 0, n0 = Id(e.n.size()); i < n0; ++i)
    if (!used[i] && e.n[i].op != Op::Var && e.n[i].op != Op::Const) {
      if (root) { e.n.push_back(bin(Op::Add, *root, i)); root = Id(e.n.size() - 1); } else root = i;
    }
  if (*root != e.n.size() - 1) e.n.push_back(bin(Op::Add, *root, *root));
  return e;
}

static bool sound(EGraph& g, const Pick& p, Id root, const Expr& e) {
  std::uint64_t s = 42;
  for (int t = 0; t < 16; ++t) {
    std::vector<std::int64_t> env(8);
    for (auto& v : env) v = std::int64_t(splitmix(s));
    if (eval(g, p, root, env) != eval(e, env)) return false;
  }
  return true;
}

struct Outcome {
  std::uint64_t graph_fp, greedy_tree, greedy_dag;
  std::size_t classes, nodes;
  std::string term;
  Stats st;
};
static Outcome run(const Expr& e, Budget b, std::uint64_t seed, std::size_t buckets, unsigned threads, Order o) {
  EGraph g(seed, buckets);
  Id root = load(g, e);
  Outcome r;
  r.st = saturate(g, rules(), b, threads, o);
  root = g.find(root);
  auto p = greedy(g, &r.greedy_tree, root);
  r.greedy_dag = dag_cost(g, p, root);
  r.graph_fp = g.fingerprint();
  r.classes = g.n_classes();
  r.nodes = g.n_nodes;
  r.term = show(g, p, root, 1u << 30);
  if (!sound(g, p, root, e)) { std::printf("UNSOUND extraction\n"); std::exit(2); }
  return r;
}
static std::uint64_t fnv(std::string_view s) {
  std::uint64_t h = 0xcbf29ce484222325;
  for (char c : s) h = (h ^ std::uint8_t(c)) * 0x100000001b3;
  return h;
}

// ---- commands -----------------------------------------------------------------------------

static int demo() {
  std::vector<std::pair<const char*, Expr>> ex;
  { B b; Id x = b.x(0); b.op(Op::Add, b.op(Op::Mul, x, b.c(3)), b.op(Op::Mul, x, b.c(5))); ex.push_back({"x*3 + x*5", b.e}); }
  { B b; Id x = b.x(0), y = b.x(1); b.op(Op::Sub, b.op(Op::Add, x, y), b.op(Op::Add, y, x)); ex.push_back({"(x+y) - (y+x)", b.e}); }
  { B b; Id x = b.x(0); b.op(Op::Sub, b.op(Op::Mul, x, b.c(3)), b.op(Op::Mul, x, b.c(6))); ex.push_back({"x*3 - x*6", b.e}); }
  { B b; Id x = b.x(0), y = b.x(1), z = b.x(2);
    Id t = b.op(Op::Mul, x, y);
    b.op(Op::Sub, b.op(Op::Sub, t, b.op(Op::Mul, b.op(Op::Mul, x, y), b.c(6))), b.op(Op::Mul, b.op(Op::Mul, t, b.c(12)), z));
    ex.push_back({"x*y - (x*y)*6 - (x*y)*12*z", b.e}); }
  { B b; Id x = b.x(0), y = b.x(1);
    Id t = b.op(Op::Mul, x, b.c(5));
    Id a = b.op(Op::Mul, x, b.c(10)), c = b.op(Op::Mul, x, b.c(20)), d = b.op(Op::Mul, x, b.c(40));
    b.op(Op::Sub, b.op(Op::Sub, b.op(Op::Sub, b.op(Op::Mul, t, y), a), c), d);
    ex.push_back({"x*5*y - x*10 - x*20 - x*40", b.e}); }
  int bad = 0;
  std::printf("%-28s %7s %7s %5s | %-28s %4s %4s | %4s %8s %s\n", "input", "classes", "nodes", "sat", "greedy", "tree", "dag",
              "ilp", "ilp ms", "ilp term");
  for (auto& [label, e] : ex) {
    EGraph g(1, 0);
    Id root = load(g, e);
    auto st = saturate(g, rules(), {30, 50000}, 1, Order::Id);
    root = g.find(root);
    std::uint64_t tree;
    auto gp = greedy(g, &tree, root);
    auto r = ilp(g, root, {});
    std::uint64_t gd = dag_cost(g, gp, root), id = dag_cost(g, r.pick, root);
    bool ok = sound(g, gp, root, e) && sound(g, r.pick, root, e) && id == std::uint64_t(r.objective + 0.5) && id <= gd;
    bad += !ok;
    std::printf("%-28s %7zu %7zu %5s | %-28s %4llu %4llu | %4llu %8.2f %s%s\n", label, g.n_classes(), g.n_nodes,
                st.saturated ? "yes" : "no", show(g, gp, root, 28).c_str(), (unsigned long long)tree, (unsigned long long)gd,
                (unsigned long long)id, r.ms, show(g, r.pick, root, 60).c_str(), ok ? "" : "  FAIL");
  }
  return bad ? 1 : 0;
}

static int random_gap(int count, int size, std::size_t cap) {
  int worse = 0, bad = 0;
  double total_ms = 0, max_ms = 0, worst = 0;
  std::uint64_t sum_g = 0, sum_i = 0;
  std::size_t max_nodes = 0, cyc = 0, loops = 0, cyc_models = 0, cyclic_picks = 0;
  for (int i = 0; i < count; ++i) {
    Expr e = random_expr(1000 + i, size, 3);
    EGraph g(1, 0);
    Id root = load(g, e);
    saturate(g, rules(), {6, cap}, 1, Order::Id);
    root = g.find(root);
    std::uint64_t tree;
    auto gp = greedy(g, &tree, root);
    HighsOpts ho;
    ho.acyclic = !std::getenv("NO_ACYCLIC"), ho.prune = !std::getenv("NO_PRUNE");
    auto r = ilp(g, root, ho);
    std::uint64_t gd = dag_cost(g, gp, root), id = dag_cost(g, r.pick, root);
    if (id == kInf) { ++cyclic_picks; continue; }  // only possible without the acyclicity rows
    bad += !(sound(g, r.pick, root, e) && id == std::uint64_t(r.objective + 0.5) && id <= gd && r.status == kHighsModelStatusOptimal);
    worse += gd > id;
    worst = std::max(worst, double(gd) / double(id) - 1);
    sum_g += gd, sum_i += id;
    total_ms += r.ms, max_ms = std::max(max_ms, r.ms);
    max_nodes = std::max(max_nodes, g.n_nodes);
    cyc += r.cyclic_classes, loops += r.self_loops, cyc_models += r.cyclic_classes > 0;
    if (std::getenv("VERBOSE"))
      std::printf("  #%d: %zu e-nodes, cols %zu rows %zu, greedy %llu ilp %llu, %.1f ms, B&B nodes %lld\n", i, g.n_nodes, r.cols, r.rows,
                  (unsigned long long)gd, (unsigned long long)id, r.ms, (long long)r.mip_nodes);
  }
  std::printf("random: %d expressions of %d input nodes, budget 6 iterations / %zu e-nodes (max %zu e-nodes)\n", count, size, cap, max_nodes);
  std::printf("  greedy DAG cost > ILP DAG cost in %d/%d; total %llu vs %llu (%.1f%%); worst single gap %.1f%%\n", worse, count,
              (unsigned long long)sum_g, (unsigned long long)sum_i, 100.0 * (double(sum_g) / double(sum_i) - 1), 100 * worst);
  std::printf("  ILP solve: mean %.1f ms, max %.1f ms; failures (unsound/non-optimal/mismatch): %d; cyclic picks: %zu\n",
              total_ms / count, max_ms, bad, cyclic_picks);
  std::printf("  cycles: %zu self-loop e-nodes dropped; %zu models had classes in cycles (%zu classes given MTZ potentials)\n", loops,
              cyc_models, cyc);
  return bad ? 1 : 0;
}

static int determinism() {
  Expr e = random_expr(7, 120, 4);
  Budget b{8, 30000};
  struct Cfg { std::uint64_t seed; std::size_t buckets; };
  Cfg cfgs[] = {{1, 0}, {1, 0}, {0xdeadbeef, 0}, {12345, 1 << 16}, {99, 7}};
  int fail = 0;
  for (Order o : {Order::Id, Order::Hash}) {
    std::printf("%s order (budget %d iterations / %zu e-nodes):\n", o == Order::Id ? "id" : "hash-table", b.iters, b.nodes);
    std::optional<std::uint64_t> first;
    bool same = true;
    for (auto [seed, buckets] : cfgs) {
      auto r = run(e, b, seed, buckets, 1, o);
      std::uint64_t fp = r.graph_fp ^ fnv(r.term) ^ (r.greedy_dag * 31);
      if (!first) first = fp;
      same &= fp == *first;
      std::printf("  seed %-10llx buckets %-6zu: classes %6zu nodes %6zu iters %d limit %d  tree %4llu dag %3llu  term %016llx  fp %016llx\n",
                  (unsigned long long)seed, buckets, r.classes, r.nodes, r.st.iters, r.st.node_limit,
                  (unsigned long long)r.greedy_tree, (unsigned long long)r.greedy_dag, (unsigned long long)fnv(r.term),
                  (unsigned long long)fp);
    }
    std::printf("  => %s\n", same ? "identical" : "DIFFERENT");
    if (o == Order::Id && !same) fail = 1;
  }
  return fail;
}

static int threads(unsigned n) {
  Expr e = random_expr(7, 120, 4);
  Budget b{8, 300000};
  std::optional<std::uint64_t> first;
  int fail = 0;
  for (unsigned t : {1u, 2u, n}) {
    auto t0 = Clock::now();
    auto r = run(e, b, 1, 0, t, Order::Id);
    std::uint64_t fp = r.graph_fp ^ fnv(r.term) ^ (r.greedy_dag * 31);
    if (!first) first = fp;
    fail |= fp != *first;
    std::printf("threads %2u: classes %6zu nodes %6zu  dag %llu  fp %016llx  %.0f ms (match %.0f, apply %.0f, rebuild %.0f)\n", t,
                r.classes, r.nodes, (unsigned long long)r.greedy_dag, (unsigned long long)fp, ms_since(t0), r.st.match_ms,
                r.st.apply_ms, r.st.rebuild_ms);
  }
  std::printf("  => %s\n", fail ? "DIFFERENT" : "identical");
  return fail;
}

static int growth(int size) {
  Expr e = random_expr(7, size, 4);
  EGraph g(1, 0);
  Id root = load(g, e);
  g.rebuild();
  std::printf("growth: %d-entry input, %zu e-nodes / %zu e-classes after hash-consing; no iteration budget, 2M e-node cap\n",
              size, g.n_nodes, g.n_classes());
  std::printf("%5s %10s %10s %10s %10s %8s %6s\n", "iter", "e-nodes", "e-classes", "matches", "ms", "tree", "dag");
  Budget b{1, 2'000'000};
  for (int it = 1; it <= 12; ++it) {
    auto t0 = Clock::now();
    auto st = saturate(g, rules(), b, 1, Order::Id);
    double ms = ms_since(t0);
    std::uint64_t tree;
    auto p = greedy(g, &tree, g.find(root));
    std::printf("%5d %10zu %10zu %10zu %10.0f %8llu %6llu%s\n", it, g.n_nodes, g.n_classes(), st.per_iter.back()[2], ms,
                (unsigned long long)tree, (unsigned long long)dag_cost(g, p, g.find(root)),
                st.saturated ? "  saturated" : st.node_limit ? "  node cap" : "");
    if (st.saturated || st.node_limit) break;
  }
  return 0;
}

static int highs(int thr, std::size_t cap) {
  // One mid-size model, solved repeatedly; then with other seeds.
  int fail = 0;
  {
    Expr e = random_expr(1000, 30, 3);
    EGraph g(1, 0);
    Id root = load(g, e);
    saturate(g, rules(), {6, cap}, 1, Order::Id);
    root = g.find(root);
    std::uint64_t tree;
    auto gp = greedy(g, &tree, root);
    std::printf("model from a 30-entry input, budget %zu: %zu e-nodes, greedy DAG %llu\n", cap, g.n_nodes, (unsigned long long)dag_cost(g, gp, root));
    std::optional<std::uint64_t> first;
    for (int seed : {0, 0, 0, 0, 0, 1, 2, 3}) {
      auto r = ilp(g, root, {thr, seed});
      std::uint64_t key = r.solution_hash ^ (std::uint64_t(r.simplex_iters) << 20) ^ std::uint64_t(r.mip_nodes);
      if (seed == 0) { if (!first) first = key; fail |= key != *first; }
      std::printf("  threads %2d seed %d: cols %5zu rows %5zu obj %5.0f status %d simplex %6lld nodes %5lld sol %016llx  %7.1f ms\n",
                  thr, seed, r.cols, r.rows, r.objective, r.status, (long long)r.simplex_iters, (long long)r.mip_nodes,
                  (unsigned long long)r.solution_hash, r.ms);
    }
  }
  std::printf("  => seed-0 runs %s\n", fail ? "DIFFERENT" : "identical");
  return fail;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string_view cmd = argc > 1 ? argv[1] : "";
  auto arg = [&](int i, int d) { return argc > i ? std::atoi(argv[i]) : d; };
  if (cmd == "demo") return demo();
  if (cmd == "random") return random_gap(arg(2, 50), arg(3, 30), std::size_t(arg(4, 300)));
  if (cmd == "determinism") return determinism();
  if (cmd == "threads") return threads(unsigned(arg(2, 8)));
  if (cmd == "growth") return growth(arg(2, 100));
  if (cmd == "highs") return highs(arg(2, 1), std::size_t(arg(3, 300)));
  std::fprintf(stderr, "usage: spike demo|random [n size]|determinism|threads [n]|growth [size]|highs [threads (-1 = default) [e-node budget]]\n");
  return 64;
}
