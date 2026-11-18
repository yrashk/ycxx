// Extraction: greedy bottom-up (tree cost) and an ILP over HiGHS (DAG cost: every chosen e-node
// is paid once, however many parents use it). Both pick at most one e-node per e-class.
#pragma once
#include "egraph.hpp"

#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <string>

#include "interfaces/highs_c_api.h"

namespace eg {

inline constexpr std::uint64_t kInf = UINT64_MAX / 4;
inline constexpr double kInfD = std::numeric_limits<double>::infinity();
using Pick = std::vector<std::optional<Node>>;  // by class id

// Fixed point over classes in id order; ties go to the smaller e-node, so the result is a
// function of the graph alone. Costs are >= 1, so a child is always strictly cheaper than its
// parent and the picked nodes cannot form a cycle.
inline Pick greedy(EGraph& g, std::uint64_t* tree_cost, Id root) {
  std::vector<std::uint64_t> best(g.uf.size(), kInf);
  Pick pick(g.uf.size());
  for (bool again = true; again;) {
    again = false;
    for (Id c = 0; c < g.uf.size(); ++c) {
      if (g.uf[c] != c) continue;
      for (const Node& n : g.cls[c].nodes) {
        std::uint64_t x = cost(n.op);
        for (int i = 0; i < arity(n.op); ++i) x = std::min(kInf, x + best[n.k[i]]);
        if (x < best[c] || (x == best[c] && x < kInf && n < *pick[c])) { best[c] = x; pick[c] = n; again = true; }
      }
    }
  }
  *tree_cost = best[g.find(root)];
  return pick;
}

// Returns kInf if the pick is incomplete or cyclic below root.
inline std::uint64_t dag_cost(EGraph& g, const Pick& p, Id root) {
  std::vector<int> state(g.uf.size());  // 0 new, 1 on stack, 2 done
  std::uint64_t total = 0;
  std::function<bool(Id)> go = [&](Id c) {
    c = g.find(c);
    if (state[c] == 2) return true;
    if (state[c] == 1 || !p[c]) return false;
    state[c] = 1;
    total += cost(p[c]->op);
    for (int i = 0; i < arity(p[c]->op); ++i) if (!go(p[c]->k[i])) return false;
    state[c] = 2;
    return true;
  };
  return go(root) ? total : kInf;
}

inline std::int64_t eval(EGraph& g, const Pick& p, Id root, const std::vector<std::int64_t>& env) {
  std::map<Id, std::int64_t> memo;
  std::function<std::int64_t(Id)> go = [&](Id c) -> std::int64_t {
    c = g.find(c);
    if (auto it = memo.find(c); it != memo.end()) return it->second;
    const Node& n = *p[c];
    std::int64_t r = n.op == Op::Var ? env[n.v]
                     : eval_op(n.op, n.v, arity(n.op) > 0 ? go(n.k[0]) : 0, arity(n.op) > 1 ? go(n.k[1]) : 0);
    return memo[c] = r;
  };
  return go(root);
}

inline std::string show(EGraph& g, const Pick& p, Id c, std::size_t cap = 160) {
  const Node& n = *p[g.find(c)];
  if (n.op == Op::Const) return std::to_string(n.v);
  if (n.op == Op::Var) return "x" + std::to_string(n.v);
  std::string s = std::string("(") + name(n.op);
  for (int i = 0; i < arity(n.op) && s.size() < cap; ++i) s += " " + show(g, p, n.k[i], cap);
  return s.size() < cap ? s + ")" : s.substr(0, cap) + "...";
}

struct IlpResult {
  Pick pick;
  double objective = 0, ms = 0;
  int status = 0;
  std::int64_t simplex_iters = 0, mip_nodes = 0;
  std::size_t cols = 0, rows = 0, cyclic_classes = 0, self_loops = 0;
  std::uint64_t solution_hash = 0;
};

struct HighsOpts { int threads = 1; int seed = 0; bool prune = true; bool acyclic = true; };  // threads < 0: leave HiGHS's default

// min sum cost(n)*x_n  s.t.  sum_{n in root} x_n = 1;  x_n <= sum_{m in child class} x_m.
// Cycles: e-nodes whose child is their own class are dropped (never part of an acyclic pick).
// For edges inside a strongly connected component of the class graph, a potential t_c in
// [0, |SCC|-1] is added with t_c >= t_d + 1 - |SCC| (1 - x_n) (Miller-Tucker-Zemlin), which
// forbids any chosen cycle. Edges between SCCs cannot close a cycle and get no row.
inline IlpResult ilp(EGraph& g, Id root, HighsOpts o) {
  root = g.find(root);
  // Reachable classes, in id order.
  std::vector<char> reach(g.uf.size());
  std::vector<Id> stack{root};
  reach[root] = 1;
  while (!stack.empty()) {
    Id c = stack.back(); stack.pop_back();
    for (const Node& n : g.cls[c].nodes)
      for (int i = 0; i < arity(n.op); ++i)
        if (!reach[n.k[i]]) { reach[n.k[i]] = 1; stack.push_back(n.k[i]); }
  }
  // Tarjan SCC over reachable classes (iteration over ids and sorted nodes: deterministic).
  std::vector<int> idx(g.uf.size(), -1), low(g.uf.size()), comp(g.uf.size(), -1);
  std::vector<int> comp_size;
  std::vector<Id> st;
  std::vector<char> on(g.uf.size());
  int counter = 0;
  std::function<void(Id)> scc = [&](Id c) {
    idx[c] = low[c] = counter++;
    st.push_back(c); on[c] = 1;
    for (const Node& n : g.cls[c].nodes)
      for (int i = 0; i < arity(n.op); ++i) {
        Id d = n.k[i];
        if (idx[d] < 0) { scc(d); low[c] = std::min(low[c], low[d]); }
        else if (on[d]) low[c] = std::min(low[c], idx[d]);
      }
    if (low[c] == idx[c]) {
      int k = int(comp_size.size()), sz = 0;
      Id d;
      do { d = st.back(); st.pop_back(); on[d] = 0; comp[d] = k; ++sz; } while (d != c);
      comp_size.push_back(sz);
    }
  };
  for (Id c = 0; c < g.uf.size(); ++c) if (reach[c] && idx[c] < 0) scc(c);

  // Columns: x_n for every kept node; then t_c for classes in a non-trivial SCC.
  // Dominance pruning (optional): drop n if the class has m with cost(m) <= cost(n) whose child
  // classes are a subset of n's. Any pick using n stays valid and no dearer with m instead. This
  // removes, among others, the mirror images commutativity creates, which otherwise make the
  // search explore symmetric twins.
  struct Col { Id cls; Node n; };
  std::vector<Col> xs;
  std::vector<std::vector<int>> of_class(g.uf.size());
  auto kids = [](const Node& n) {
    std::vector<Id> k(n.k.begin(), n.k.begin() + arity(n.op));
    std::ranges::sort(k);
    k.erase(std::unique(k.begin(), k.end()), k.end());
    return k;
  };
  std::size_t self_loops = 0;
  for (Id c = 0; c < g.uf.size(); ++c) {
    if (!reach[c]) continue;
    std::vector<std::pair<std::uint64_t, Node>> cand;
    for (const Node& n : g.cls[c].nodes) {
      auto k = kids(n);
      if (std::ranges::find(k, c) == k.end()) cand.push_back({cost(n.op) * 4 + k.size(), n});
      else ++self_loops;
    }
    std::ranges::sort(cand);  // by cost, then child count, then node: deterministic
    std::vector<Node> kept;
    for (auto& [key, n] : cand) {
      auto kn = kids(n);
      bool dominated = o.prune && std::ranges::any_of(kept, [&](const Node& m) {
        auto km = kids(m);
        return cost(m.op) <= cost(n.op) && std::ranges::includes(kn, km);
      });
      if (dominated) continue;
      kept.push_back(n);
      of_class[c].push_back(int(xs.size()));
      xs.push_back({c, n});
    }
  }
  std::vector<int> tcol(g.uf.size(), -1);
  int ncol = int(xs.size());
  std::size_t ntcol = 0;
  for (Id c = 0; c < g.uf.size(); ++c) if (reach[c] && comp_size[comp[c]] > 1) tcol[c] = ncol++, ++ntcol;

  std::vector<double> cost_v(ncol, 0), lo(ncol, 0), hi(ncol, 1), rlo, rhi, val;
  std::vector<HighsInt> integ(ncol, 1), start{0}, index;
  for (std::size_t j = 0; j < xs.size(); ++j) cost_v[j] = double(cost(xs[j].n.op));
  for (Id c = 0; c < g.uf.size(); ++c)
    if (tcol[c] >= 0) { hi[tcol[c]] = comp_size[comp[c]] - 1; integ[tcol[c]] = 0; }
  auto row = [&](std::vector<std::pair<int, double>> e, double l, double h) {
    std::ranges::sort(e);
    for (auto [j, a] : e) { index.push_back(j); val.push_back(a); }
    start.push_back(HighsInt(index.size()));
    rlo.push_back(l); rhi.push_back(h);
  };
  {
    std::vector<std::pair<int, double>> e;
    for (int j : of_class[root]) e.push_back({j, 1});
    row(e, 1, 1);
  }
  for (std::size_t j = 0; j < xs.size(); ++j) {
    const auto& [c, n] = xs[j];
    for (int i = 0; i < arity(n.op); ++i) {
      Id d = n.k[i];
      if (i == 1 && d == n.k[0]) continue;  // same child twice: one row is enough
      std::vector<std::pair<int, double>> e{{int(j), -1}};
      for (int m : of_class[d]) e.push_back({m, 1});
      row(e, 0, kInfD);
      if (o.acyclic && comp[c] == comp[d] && tcol[c] >= 0) {  // t_c - t_d - N x_n >= 1 - N
        double N = comp_size[comp[c]];
        row({{tcol[c], 1}, {tcol[d], -1}, {int(j), -N}}, 1 - N, kInfD);
      }
    }
  }
  IlpResult r;
  r.cols = ncol;
  r.rows = rlo.size();
  r.cyclic_classes = ntcol;
  r.self_loops = self_loops;
  void* h = Highs_create();
  Highs_setBoolOptionValue(h, "output_flag", 0);
  if (o.threads >= 0) Highs_setIntOptionValue(h, "threads", o.threads);
  Highs_setIntOptionValue(h, "random_seed", o.seed);
  Highs_setDoubleOptionValue(h, "mip_rel_gap", 0);
  Highs_passMip(h, ncol, HighsInt(rlo.size()), HighsInt(index.size()), kHighsMatrixFormatRowwise,
                kHighsObjSenseMinimize, 0, cost_v.data(), lo.data(), hi.data(), rlo.data(), rhi.data(),
                start.data(), index.data(), val.data(), integ.data());
  auto t0 = std::chrono::steady_clock::now();
  Highs_run(h);
  r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  r.status = int(Highs_getModelStatus(h));
  std::vector<double> col(ncol), cdual(ncol), rv(rlo.size()), rdual(rlo.size());
  Highs_getSolution(h, col.data(), cdual.data(), rv.data(), rdual.data());
  r.objective = Highs_getObjectiveValue(h);
  HighsInt it = 0;
  Highs_getIntInfoValue(h, "simplex_iteration_count", &it);
  r.simplex_iters = it;
  int64_t nodes = 0;
  Highs_getInt64InfoValue(h, "mip_node_count", &nodes);
  r.mip_nodes = nodes;
  Highs_destroy(h);
  r.pick.resize(g.uf.size());
  r.solution_hash = 0xcbf29ce484222325;
  for (std::size_t j = 0; j < xs.size(); ++j)
    if (col[j] > 0.5) {
      if (!r.pick[xs[j].cls]) r.pick[xs[j].cls] = xs[j].n;
      r.solution_hash = (r.solution_hash ^ j) * 0x100000001b3;
    }
  return r;
}

}  // namespace eg
