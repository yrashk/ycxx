// Throwaway spike for yrashk/ycxx#3: a toy e-graph after the published egg design
// (Willsey et al., POPL 2021): union-find with canonical ids, hash-consed e-nodes, deferred
// congruence closure ("rebuild"), and a constant-folding e-class analysis. Written from the paper,
// not from any implementation.
//
// Determinism rule: every order that reaches the result comes from ids, and ids are handed out by
// a counter in the order the deterministic algorithm adds nodes. The hash table is only probed
// with exact keys, never iterated (except in `Order::Hash`, the deliberate counter-example).
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eg {

using Id = std::uint32_t;
inline constexpr Id kNone = UINT32_MAX;

enum class Op : std::uint8_t { Const, Var, Neg, Add, Sub, Mul, Shl };
constexpr int arity(Op o) { return o == Op::Const || o == Op::Var ? 0 : o == Op::Neg ? 1 : 2; }
constexpr const char* name(Op o) {
  constexpr const char* n[] = {"c", "v", "neg", "+", "-", "*", "<<"};
  return n[int(o)];
}
constexpr std::uint64_t cost(Op o) {
  constexpr std::uint64_t c[] = {1, 1, 1, 2, 2, 4, 2};
  return c[int(o)];
}

struct Node {
  Op op{};
  std::int64_t v = 0;  // constant value or variable index
  std::array<Id, 2> k{0, 0};
  auto operator<=>(const Node&) const = default;
};
inline Node cnst(std::int64_t v) { return {Op::Const, v, {0, 0}}; }
inline Node var(int i) { return {Op::Var, i, {0, 0}}; }
inline Node un(Op o, Id a) { return {o, 0, {a, 0}}; }
inline Node bin(Op o, Id a, Id b) { return {o, 0, {a, b}}; }

// i64 with wrapping semantics; shift amounts are taken mod 64.
inline std::int64_t eval_op(Op o, std::int64_t v, std::int64_t a, std::int64_t b) {
  using U = std::uint64_t;
  switch (o) {
    case Op::Const: return v;
    case Op::Var: std::abort();
    case Op::Neg: return std::int64_t(U(0) - U(a));
    case Op::Add: return std::int64_t(U(a) + U(b));
    case Op::Sub: return std::int64_t(U(a) - U(b));
    case Op::Mul: return std::int64_t(U(a) * U(b));
    case Op::Shl: return std::int64_t(U(a) << (U(b) & 63));
  }
  std::abort();
}

struct NodeHash {
  std::uint64_t seed;
  std::size_t operator()(const Node& n) const noexcept {
    std::uint64_t h = seed;
    for (std::uint64_t x : {std::uint64_t(n.op), std::uint64_t(n.v), std::uint64_t(n.k[0]),
                            std::uint64_t(n.k[1])}) {
      h ^= x + 0x9e3779b97f4a7c15 + (h << 6) + (h >> 2);
      h *= 0xff51afd7ed558ccd;
      h ^= h >> 33;
    }
    return h;
  }
};

struct Class {
  std::vector<Node> nodes;                     // canonical, sorted, unique after rebuild()
  std::vector<std::pair<Node, Id>> parents;    // nodes that use this class, and their class
  std::optional<std::int64_t> k;               // analysis: the class's constant value
};

enum class Order { Id, Hash };  // Hash: enumerate classes by iterating the hash table (wrong)

struct EGraph {
  std::vector<Id> uf;
  std::vector<Class> cls;  // indexed by id; only roots are live
  std::unordered_map<Node, Id, NodeHash> memo;
  std::vector<Id> pending;
  std::size_t n_nodes = 0;
  bool changed = false;

  EGraph(std::uint64_t seed, std::size_t buckets) : memo(buckets, NodeHash{seed}) {}

  Id find(Id a) {
    while (uf[a] != a) a = uf[a] = uf[uf[a]];
    return a;
  }
  Node canon(Node n) {
    for (int i = 0; i < arity(n.op); ++i) n.k[i] = find(n.k[i]);
    return n;
  }
  std::optional<std::int64_t> make(const Node& n) {
    if (n.op == Op::Const) return n.v;
    if (n.op == Op::Var) return std::nullopt;
    std::int64_t a = 0, b = 0;
    if (auto x = cls[find(n.k[0])].k) a = *x; else return std::nullopt;
    if (arity(n.op) == 2) { if (auto y = cls[find(n.k[1])].k) b = *y; else return std::nullopt; }
    return eval_op(n.op, 0, a, b);
  }
  std::optional<std::int64_t> konst(Id a) { return cls[find(a)].k; }

  Id add(Node n) {
    n = canon(n);
    if (auto it = memo.find(n); it != memo.end()) return find(it->second);
    Id id = Id(uf.size());
    uf.push_back(id);
    cls.push_back(Class{{n}, {}, make(n)});
    for (int i = 0; i < arity(n.op); ++i) cls[n.k[i]].parents.push_back({n, id});
    memo.emplace(n, id);
    ++n_nodes;
    changed = true;
    modify(id);
    return find(id);
  }

  // egg's `modify`: a class with a known constant also gets the constant's e-node.
  void modify(Id id) {
    id = find(id);
    if (!cls[id].k) return;
    for (const Node& n : cls[id].nodes) if (n.op == Op::Const) return;
    Id c = add(cnst(*cls[id].k));
    merge(id, c);
  }

  bool merge(Id a, Id b) {
    a = find(a), b = find(b);
    if (a == b) return false;
    if (b < a) std::swap(a, b);  // the smaller id stays canonical: a stable key, not a pointer
    uf[b] = a;
    Class& A = cls[a];
    Class& B = cls[b];
    A.nodes.insert(A.nodes.end(), B.nodes.begin(), B.nodes.end());
    A.parents.insert(A.parents.end(), B.parents.begin(), B.parents.end());
    if (A.k && B.k && *A.k != *B.k) { std::fprintf(stderr, "unsound merge\n"); std::abort(); }
    if (!A.k) A.k = B.k;
    B = Class{};
    pending.push_back(a);
    changed = true;
    return true;
  }

  void repair(Id c) {
    auto ps = std::move(cls[c].parents);
    cls[c].parents.clear();
    for (auto& [n, id] : ps) memo.erase(n);
    for (auto& [n, id] : ps) {
      n = canon(n);
      auto [it, fresh] = memo.try_emplace(n, find(id));
      if (!fresh) merge(it->second, id);  // congruence: same canonical node in two classes
      it->second = find(id);
    }
    std::ranges::sort(ps);
    std::vector<std::pair<Node, Id>> kept;
    for (auto& [n, id] : ps) {
      if (!kept.empty() && kept.back().first == n) merge(kept.back().second, id);
      else kept.push_back({n, id});
    }
    for (auto& [n, id] : kept) {  // analysis propagation upward
      Id p = find(id);
      if (auto d = make(n); d && !cls[p].k) { cls[p].k = d; pending.push_back(p); }
    }
    Id r = find(c);
    cls[r].parents.insert(cls[r].parents.end(), kept.begin(), kept.end());
  }

  void rebuild() {
    while (!pending.empty()) {
      auto todo = std::move(pending);
      pending.clear();
      for (Id& id : todo) id = find(id);
      std::ranges::sort(todo);
      todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
      for (Id c : todo) repair(find(c));
      for (Id c : todo) modify(c);
    }
    // Housekeeping (a simplification egg avoids): re-canonicalize every class and rebuild the
    // hash-cons from scratch in id order. It also checks the congruence invariant.
    memo.clear();
    n_nodes = 0;
    for (Id id = 0; id < uf.size(); ++id) {
      if (uf[id] != id) continue;
      auto& v = cls[id].nodes;
      for (Node& n : v) n = canon(n);
      std::ranges::sort(v);
      v.erase(std::unique(v.begin(), v.end()), v.end());
      for (const Node& n : v)
        if (!memo.try_emplace(n, id).second) { std::fprintf(stderr, "congruence broken\n"); std::abort(); }
      n_nodes += v.size();
      auto& p = cls[id].parents;
      for (auto& [n, c] : p) n = canon(n), c = find(c);
      std::ranges::sort(p);
      p.erase(std::unique(p.begin(), p.end()), p.end());
    }
  }

  std::size_t n_classes() const {
    std::size_t c = 0;
    for (Id i = 0; i < uf.size(); ++i) c += uf[i] == i;
    return c;
  }

  // The classes matching visits, in the order it visits them.
  std::vector<Id> class_order(Order o) const {
    std::vector<Id> out;
    if (o == Order::Id) {
      for (Id i = 0; i < uf.size(); ++i) if (uf[i] == i) out.push_back(i);
    } else {
      std::vector<bool> seen(uf.size());
      for (auto& [n, id] : memo) if (!seen[id]) { seen[id] = true; out.push_back(id); }
    }
    return out;
  }

  // FNV-1a over the whole canonical graph: ids, analysis data and e-nodes.
  std::uint64_t fingerprint() const {
    std::uint64_t h = 0xcbf29ce484222325;
    auto mix = [&](std::uint64_t x) { for (int i = 0; i < 8; ++i) { h ^= (x >> (8 * i)) & 0xff; h *= 0x100000001b3; } };
    for (Id i = 0; i < uf.size(); ++i) {
      if (uf[i] != i) continue;
      mix(i);
      mix(cls[i].k ? std::uint64_t(*cls[i].k) : 0xdead);
      for (const Node& n : cls[i].nodes) { mix(std::uint64_t(n.op)); mix(std::uint64_t(n.v)); mix(n.k[0]); mix(n.k[1]); }
    }
    return h;
  }
};

// ---- Patterns and rules -------------------------------------------------------------------

struct Pat {
  Op op{};
  int var = -1;  // >= 0: pattern variable
  std::vector<Pat> kids;
};
inline Pat V(int i) { return {Op::Var, i, {}}; }
inline Pat P(Op o, Pat a) { return {o, -1, {std::move(a)}}; }
inline Pat P(Op o, Pat a, Pat b) { return {o, -1, {std::move(a), std::move(b)}}; }

using Subst = std::array<Id, 3>;
struct Rule {
  const char* name;
  Pat lhs;
  std::function<std::optional<Id>(EGraph&, const Subst&)> rhs;  // may refuse (a condition)
};
struct Match { Id cls; Subst s; };

// Read-only: safe to run for several rules at once.
inline void ematch(const EGraph& g, const Pat& p, Id c, const Subst& s, std::vector<Subst>& out) {
  if (p.var >= 0) {
    if (s[p.var] == kNone) { Subst t = s; t[p.var] = c; out.push_back(t); }
    else if (s[p.var] == c) out.push_back(s);
    return;
  }
  for (const Node& n : g.cls[c].nodes) {
    if (n.op != p.op) continue;
    std::vector<Subst> cur{s};
    for (std::size_t i = 0; i < p.kids.size() && !cur.empty(); ++i) {
      std::vector<Subst> next;
      for (const Subst& t : cur) ematch(g, p.kids[i], n.k[i], t, next);
      cur = std::move(next);
    }
    out.insert(out.end(), cur.begin(), cur.end());
  }
}

inline std::vector<Rule> rules() {
  using enum Op;
  auto pow2 = [](std::int64_t k) { return k > 1 && std::has_single_bit(std::uint64_t(k)); };
  return {
      {"add-comm", P(Add, V(0), V(1)), [](EGraph& g, const Subst& s) { return g.add(bin(Add, s[1], s[0])); }},
      {"mul-comm", P(Mul, V(0), V(1)), [](EGraph& g, const Subst& s) { return g.add(bin(Mul, s[1], s[0])); }},
      {"add-assoc", P(Add, V(0), P(Add, V(1), V(2))),
       [](EGraph& g, const Subst& s) { return g.add(bin(Add, g.add(bin(Add, s[0], s[1])), s[2])); }},
      {"mul-assoc", P(Mul, V(0), P(Mul, V(1), V(2))),
       [](EGraph& g, const Subst& s) { return g.add(bin(Mul, g.add(bin(Mul, s[0], s[1])), s[2])); }},
      {"factor", P(Add, P(Mul, V(0), V(1)), P(Mul, V(0), V(2))),
       [](EGraph& g, const Subst& s) { return g.add(bin(Mul, s[0], g.add(bin(Add, s[1], s[2])))); }},
      {"add-self", P(Add, V(0), V(0)), [](EGraph& g, const Subst& s) { return g.add(bin(Mul, s[0], g.add(cnst(2)))); }},
      {"sub-self", P(Sub, V(0), V(0)), [](EGraph& g, const Subst&) { return g.add(cnst(0)); }},
      {"sub-neg", P(Sub, V(0), V(1)), [](EGraph& g, const Subst& s) { return g.add(bin(Add, s[0], g.add(un(Neg, s[1])))); }},
      {"neg-neg", P(Neg, P(Neg, V(0))), [](EGraph&, const Subst& s) { return std::optional<Id>(s[0]); }},
      {"mul-pow2", P(Mul, V(0), V(1)),
       [=](EGraph& g, const Subst& s) -> std::optional<Id> {
         auto k = g.konst(s[1]);
         if (!k || !pow2(*k)) return std::nullopt;
         return g.add(bin(Shl, s[0], g.add(cnst(std::countr_zero(std::uint64_t(*k))))));
       }},
      {"mul-even", P(Mul, V(0), V(1)),  // x*2c -> (x*c)<<1
       [=](EGraph& g, const Subst& s) -> std::optional<Id> {
         auto k = g.konst(s[1]);
         if (!k || *k == 0 || *k % 2 != 0 || pow2(*k) || g.konst(s[0])) return std::nullopt;
         return g.add(bin(Shl, g.add(bin(Mul, s[0], g.add(cnst(*k / 2)))), g.add(cnst(1))));
       }},
      {"add-zero", P(Add, V(0), V(1)),
       [](EGraph& g, const Subst& s) -> std::optional<Id> {
         if (g.konst(s[1]) != 0) return std::nullopt;
         return s[0];
       }},
      {"mul-one-zero", P(Mul, V(0), V(1)),
       [](EGraph& g, const Subst& s) -> std::optional<Id> {
         auto k = g.konst(s[1]);
         if (k == 1) return s[0];
         if (k == 0) return g.add(cnst(0));
         return std::nullopt;
       }},
  };
}

// ---- Saturation ---------------------------------------------------------------------------

struct Budget { int iters; std::size_t nodes; };
struct Stats {
  int iters = 0;
  bool saturated = false, node_limit = false;
  std::vector<std::array<std::size_t, 3>> per_iter;  // e-nodes, e-classes, matches
  double match_ms = 0, apply_ms = 0, rebuild_ms = 0;
};

inline std::vector<Match> match_rule(const EGraph& g, const Rule& r, const std::vector<Id>& order) {
  std::vector<Match> out;
  std::vector<Subst> ss;
  for (Id c : order) {
    ss.clear();
    ematch(g, r.lhs, c, Subst{kNone, kNone, kNone}, ss);
    for (const Subst& s : ss) out.push_back({c, s});
  }
  return out;
}

inline Stats saturate(EGraph& g, const std::vector<Rule>& rs, Budget b, unsigned threads, Order order) {
  Stats st;
  g.rebuild();
  st.per_iter.push_back({g.n_nodes, g.n_classes(), 0});
  for (; st.iters < b.iters; ) {
    ++st.iters;
    using C = std::chrono::steady_clock;
    auto dt = [](C::time_point a, C::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    auto t0 = C::now();
    auto visit = g.class_order(order);
    // Match phase: read-only, one result slot per rule, so thread count cannot reorder matches.
    std::vector<std::vector<Match>> ms(rs.size());
    if (threads <= 1) {
      for (std::size_t r = 0; r < rs.size(); ++r) ms[r] = match_rule(g, rs[r], visit);
    } else {
      std::vector<std::jthread> pool;
      for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&, t] { for (std::size_t r = t; r < rs.size(); r += threads) ms[r] = match_rule(g, rs[r], visit); });
    }
    // Apply phase: sequential, rule order then match order. The node budget is checked before
    // each application, so where it stops is a function of that order alone.
    auto t1 = C::now();
    g.changed = false;
    std::size_t n_matches = 0;
    for (std::size_t r = 0; r < rs.size() && !st.node_limit; ++r)
      for (const Match& m : ms[r]) {
        if (g.n_nodes >= b.nodes) { st.node_limit = true; break; }
        ++n_matches;
        if (auto id = rs[r].rhs(g, m.s)) g.merge(m.cls, *id);
      }
    auto t2 = C::now();
    g.rebuild();
    auto t3 = C::now();
    st.match_ms += dt(t0, t1), st.apply_ms += dt(t1, t2), st.rebuild_ms += dt(t2, t3);
    st.per_iter.push_back({g.n_nodes, g.n_classes(), n_matches});
    if (!g.changed) { st.saturated = true; break; }
    if (st.node_limit) break;
  }
  return st;
}

}  // namespace eg
