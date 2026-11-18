// AST representation spike for yrashk/ycxx#2. Throwaway: see README.md for what it settles.
// Plain headers rather than modules and libycxx: the build skeleton (#4) does not exist yet.
#include <meta>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ast {
using u8 = std::uint8_t; using u32 = std::uint32_t; using u64 = std::uint64_t;

// ---------------------------------------------------------------- fingerprints (spike-quality mix)
struct Fp { u64 lo = 0, hi = 0; friend constexpr bool operator==(Fp, Fp) = default; };
constexpr u64 fmix(u64 k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
                            k *= 0xc4ceb9fe1a85ec53ULL; k ^= k >> 33; return k; }
constexpr Fp mix(Fp h, u64 v) { h.lo = fmix(h.lo ^ v ^ 0x9e3779b97f4a7c15ULL);
                                h.hi = fmix(h.hi + v * 0xc2b2ae3d27d4eb4fULL + h.lo); return h; }
constexpr Fp mix(Fp h, Fp v) { return mix(mix(h, v.lo), v.hi); }
consteval u64 str_hash(std::string_view s) { u64 h = 1469598103934665603ULL;
  for (char c : s) { h ^= u8(c); h *= 1099511628211ULL; } return h; }
template<class T> consteval Fp type_seed() { return mix(Fp{}, str_hash(std::meta::display_string_of(^^T))); }

// ---------------------------------------------------------------- reflection helpers
template<class T> consteval auto members() {
  return std::define_static_array(
      std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
}
struct no_fp_t {};   inline constexpr no_fp_t no_fp{};    // derived cache: not content
struct sugar_t {};   inline constexpr sugar_t sugar{};    // spelling only: ignored by canonical equality
template<class Tag> consteval bool has(std::meta::info m) {
  return std::meta::annotations_of_with_type(m, ^^Tag).size() > 0;
}
template<class... Ts> struct kinds {
  static constexpr u32 size = sizeof...(Ts);
  template<u32 I> using at = Ts...[I];                    // pack indexing (P2662)
  template<class T> static consteval u32 index_of() {
    u32 i = 0, r = size; ((std::is_same_v<T, Ts> ? (r = i, ++i) : ++i), ...); return r;
  }
};

// ---------------------------------------------------------------- handles
enum class NodeId : u32 { none = 0xffffffff };   // kind in the top 8 bits, index in the low 24
enum class NameId : u32 {};
constexpr u32 kind_of(NodeId n) { return u32(n) >> 24; }
constexpr u32 index_of(NodeId n) { return u32(n) & 0xffffff; }
// A child edge holds the child's token offset relative to the parent's start. The child itself
// holds no position, so equal subtrees are one node wherever they occur.
struct Child { NodeId id; u32 at; };
struct ChildList { u32 off, len; };                      // into the interner's edge pool

// ---------------------------------------------------------------- a few node kinds
enum class BinOp : u8 { add, mul };
struct IntLit  { u64 value; };
struct NameRef { NameId name; };
struct Binary  { BinOp op; Child lhs, rhs; };
struct Call    { Child callee; ChildList args; [[=no_fp]] u32 visits; };
struct Return  { Child value; };
struct Block   { ChildList stmts; };
using Nodes = kinds<IntLit, NameRef, Binary, Call, Return, Block>;

// ---------------------------------------------------------------- the interner (hash-consing)
template<class Ks> struct Interner;
template<class... Ts> struct Interner<kinds<Ts...>> {
  using K = kinds<Ts...>;
  // One dense pool per kind. Nodes are trivially copyable aggregates, copied in and out. (A
  // std::tuple of 240 vectors cost 13 s of front-end time on its own: overload resolution in get.)
  std::array<std::vector<std::byte>, K::size> store;
  std::array<std::vector<Fp>, K::size> fps;             // per kind, parallel to store
  std::array<std::vector<u32>, K::size> widths;          // token count of the node
  std::vector<Child> edges;
  std::unordered_map<u64, std::vector<NodeId>> table;    // probed only, never iterated

  std::span<const Child> list(ChildList l) const { return {edges.data() + l.off, l.len}; }
  ChildList make_list(std::initializer_list<Child> cs) {
    ChildList l{u32(edges.size()), u32(cs.size())}; edges.insert(edges.end(), cs); return l;
  }
  template<class T> T get(NodeId n) const {
    T v; std::memcpy(&v, store[kind_of(n)].data() + std::size_t(index_of(n)) * sizeof(T), sizeof(T)); return v;
  }

  Fp fp(NodeId n) const { return fps[kind_of(n)][index_of(n)]; }   // Merkle: content + children's fps
  u32 width(NodeId n) const { return widths[kind_of(n)][index_of(n)]; }

  // Fingerprint and equality of one field, by type; one generic definition for all node kinds.
  Fp fp_field(Fp h, Child c) const { return c.id == NodeId::none ? mix(h, ~0ULL) : mix(mix(h, fp(c.id)), c.at); }
  Fp fp_field(Fp h, ChildList l) const { h = mix(h, l.len); for (Child c : list(l)) h = fp_field(h, c); return h; }
  template<class F> requires (std::is_integral_v<F> || std::is_enum_v<F>)
  Fp fp_field(Fp h, F v) const { return mix(h, u64(v)); }
  template<class T> Fp fp_node(const T& v) const {
    Fp h = type_seed<T>();
    template for (constexpr auto m : members<T>())
      if constexpr (!has<no_fp_t>(m)) h = fp_field(h, v.[:m:]);
    return h;
  }
  static bool eq_field(const Interner&, Child a, Child b) { return a.id == b.id && a.at == b.at; }
  static bool eq_field(const Interner& in, ChildList a, ChildList b) {
    if (a.len != b.len) return false;
    for (u32 i = 0; i < a.len; ++i) if (!eq_field(in, in.list(a)[i], in.list(b)[i])) return false;
    return true;
  }
  template<class F> requires (std::is_integral_v<F> || std::is_enum_v<F>)
  static bool eq_field(const Interner&, F a, F b) { return a == b; }
  template<class T> bool eq_node(const T& a, const T& b) const {
    bool r = true;
    template for (constexpr auto m : members<T>())
      if constexpr (!has<no_fp_t>(m)) r = r && eq_field(*this, a.[:m:], b.[:m:]);
    return r;
  }

  template<class T> NodeId intern(const T& v, u32 width) {
    constexpr u32 k = K::template index_of<T>();
    static_assert(k < K::size, "not a node kind");
    Fp h = mix(fp_node(v), width);
    auto& bucket = table[h.lo];
    for (NodeId n : bucket)
      if (kind_of(n) == k && eq_node(get<T>(n), v) && this->width(n) == width) return n;
    auto& pool = store[k];
    NodeId n = NodeId((k << 24) | u32(pool.size() / sizeof(T)));
    pool.resize(pool.size() + sizeof(T));
    std::memcpy(pool.data() + pool.size() - sizeof(T), &v, sizeof(T));
    fps[k].push_back(h);
    widths[k].push_back(width);
    bucket.push_back(n);
    return n;
  }

  // Dispatch on kind: a table of function pointers generated from the kinds list.
  template<class F> void visit(NodeId n, F&& f) const {
    using Fn = void (*)(const Interner&, NodeId, F&);
    static constexpr std::array<Fn, K::size> table = []<u32... I>(std::integer_sequence<u32, I...>) {
      return std::array<Fn, K::size>{ +[](const Interner& in, NodeId n, F& f) {
        using T = typename K::template at<I>; f(in.template get<T>(n)); }... };
    }(std::make_integer_sequence<u32, K::size>{});
    table[kind_of(n)](*this, n, f);
  }
};
using AstInterner = Interner<Nodes>;

// ---------------------------------------------------------------- the red view (never stored)
struct Red {
  const AstInterner* in; NodeId id; u32 offset;          // offset from the item's start
  const Red* parent;                                     // the walk's own stack, not the node's
  template<class F> void for_children(F&& f) const {
    in->visit(id, [&]<class T>(const T& v) {
      template for (constexpr auto m : members<T>()) {
        using M = [:std::meta::type_of(m):];
        if constexpr (std::is_same_v<M, Child>) {
          if (v.[:m:].id != NodeId::none) f(Red{in, v.[:m:].id, offset + v.[:m:].at, this});
        } else if constexpr (std::is_same_v<M, ChildList>) {
          for (Child c : in->list(v.[:m:])) f(Red{in, c.id, offset + c.at, this});
        }
      }
    });
  }
};

// ---------------------------------------------------------------- types
// QualType: the type node's index in the top 29 bits, cv in the low 3. A canonical type is a
// QualType whose node is canonical; sugar nodes point at what they spell.
enum class TypeNode : u32 {};
struct QualType {
  u32 bits;
  static constexpr u32 C = 1, V = 2;
  constexpr TypeNode node() const { return TypeNode(bits >> 3); }
  constexpr u32 cv() const { return bits & 7; }
  constexpr QualType with(u32 q) const { return {bits | q}; }
  friend constexpr bool operator==(QualType, QualType) = default;
};
enum class Builtin : u8 { int_, char_, void_ };
struct TBuiltin { Builtin b; };
struct TPointer { QualType pointee; };
struct TArray   { QualType elem; u64 bound; };
struct TTypedef { NameId name; [[=sugar]] QualType underlying; };   // sugar: canonical() looks through
using TypeKinds = kinds<TBuiltin, TPointer, TArray, TTypedef>;

struct TypeTable {
  struct Entry { u32 kind; u32 idx; Fp fp; };
  std::tuple<std::vector<TBuiltin>, std::vector<TPointer>, std::vector<TArray>, std::vector<TTypedef>> store;
  std::vector<Entry> nodes;
  std::unordered_map<u64, std::vector<u32>> table;

  Fp fp(QualType q) const { return mix(nodes[u32(q.node())].fp, q.cv()); }
  Fp fp_field(Fp h, QualType q) const { return mix(h, fp(q)); }
  template<class F> requires (std::is_integral_v<F> || std::is_enum_v<F>)
  Fp fp_field(Fp h, F v) const { return mix(h, u64(v)); }
  template<class T> QualType make(const T& v) {
    constexpr u32 k = TypeKinds::index_of<T>();
    Fp h = type_seed<T>();
    template for (constexpr auto m : members<T>()) h = fp_field(h, v.[:m:]);
    for (u32 n : table[h.lo]) if (nodes[n].fp == h) return {n << 3};   // spike: fp equality suffices
    auto& vec = std::get<k>(store);
    nodes.push_back({k, u32(vec.size()), h}); vec.push_back(v);
    table[h.lo].push_back(u32(nodes.size() - 1));
    return {u32(nodes.size() - 1) << 3};
  }
  template<class T> const T& get(QualType q) const { return std::get<TypeKinds::index_of<T>()>(store)[nodes[u32(q.node())].idx]; }
  u32 kind(QualType q) const { return nodes[u32(q.node())].kind; }

  // [basic.type.qualifier]: cv applied to an array type applies to its elements. Canonicalization
  // strips sugar and pushes cv down, so equal types have equal handles regardless of spelling.
  QualType canonical(QualType q) {
    switch (kind(q)) {
      case TypeKinds::index_of<TTypedef>(): return canonical(get<TTypedef>(q).underlying.with(q.cv()));
      case TypeKinds::index_of<TPointer>(): {
        QualType p = make(TPointer{canonical(get<TPointer>(q).pointee)}); return p.with(q.cv()); }
      case TypeKinds::index_of<TArray>(): {
        auto a = get<TArray>(q);
        return make(TArray{canonical(a.elem.with(q.cv())), a.bound}); }
      default: return q;
    }
  }
};
}  // namespace ast

// ---------------------------------------------------------------- dispatch cost, at a realistic kind count
namespace bench {
using namespace ast;
template<u32 N> struct K { u32 a; Child b; };
template<class Seq> struct many;
template<u32... I> struct many<std::integer_sequence<u32, I...>> { using type = kinds<K<I>...>; };
#ifndef SPIKE_NK
#define SPIKE_NK 240
#endif
constexpr u32 NK = SPIKE_NK;                                  // order of a C++ AST's expr+stmt+decl+type kinds
using ManyKinds = many<std::make_integer_sequence<u32, NK>>::type;
using ManyInterner = Interner<ManyKinds>;
}

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
  using namespace ast;
  // 1. Hash-consing: `f(x + 1) ; return x + 1;` shares the `x + 1` subtree; fps are Merkle.
  auto build = [](AstInterner& in, bool reverse) {
    // Interning order differs between the two runs; fingerprints must not.
    NodeId one, x;
    if (reverse) { one = in.intern(IntLit{1}, 1); x = in.intern(NameRef{NameId(7)}, 1); }
    else         { x = in.intern(NameRef{NameId(7)}, 1); one = in.intern(IntLit{1}, 1); }
    NodeId sum  = in.intern(Binary{BinOp::add, {x, 0}, {one, 2}}, 3);
    NodeId f    = in.intern(NameRef{NameId(3)}, 1);
    NodeId call = in.intern(Call{{f, 0}, in.make_list({{sum, 2}}), 0}, 6);
    NodeId sum2 = in.intern(Binary{BinOp::add, {x, 0}, {one, 2}}, 3);
    NodeId ret  = in.intern(Return{{sum2, 1}}, 5);
    NodeId blk  = in.intern(Block{in.make_list({{call, 1}, {ret, 8}})}, 14);
    return std::tuple{blk, sum, sum2};
  };
  AstInterner a, b;
  auto [blk_a, sum_a, sum2_a] = build(a, false);
  auto [blk_b, sum_b, sum2_b] = build(b, true);
  CHECK(sum_a == sum2_a);                                // shared subtree, one node
  CHECK(a.fp(blk_a) == b.fp(blk_b));                     // order-independent fingerprint
  CHECK(u32(a.get<Block>(blk_a).stmts.len) == 2);
  // The no_fp field does not take part: a different cache value interns to the same node.
  NodeId f = a.intern(NameRef{NameId(3)}, 1);
  NodeId one = a.intern(IntLit{1}, 1), x = a.intern(NameRef{NameId(7)}, 1);
  NodeId sum = a.intern(Binary{BinOp::add, {x, 0}, {one, 2}}, 3);
  NodeId c1 = a.intern(Call{{f, 0}, a.make_list({{sum, 2}}), 0}, 6);
  NodeId c2 = a.intern(Call{{f, 0}, a.make_list({{sum, 2}}), 99}, 6);
  CHECK(c1 == c2);

  // 2. The red view computes item-relative offsets of a shared node at each of its occurrences.
  std::vector<u32> lit_offsets;
  auto walk = [&](auto& self, const Red& r) -> void {
    if (kind_of(r.id) == Nodes::index_of<IntLit>()) lit_offsets.push_back(r.offset);
    r.for_children([&](const Red& c) { self(self, c); });
  };
  walk(walk, Red{&a, blk_a, 0, nullptr});
  // block@0: call@1 (callee@1, sum@3: x@3, one@5), return@8 (sum@9: x@9, one@11)
  CHECK(lit_offsets.size() == 2 && lit_offsets[0] == 5 && lit_offsets[1] == 11);

  // 3. Types: canonical form independent of spelling and of interning order.
  auto types = [](TypeTable& t, bool reverse) {
    QualType i, arr_i;
    if (reverse) { arr_i = t.make(TArray{t.make(TBuiltin{Builtin::int_}).with(QualType::C), 3}); i = t.make(TBuiltin{Builtin::int_}); }
    else         { i = t.make(TBuiltin{Builtin::int_}); arr_i = t.make(TArray{i.with(QualType::C), 3}); }
    QualType A  = t.make(TTypedef{NameId(1), t.make(TArray{i, 3})});        // typedef int A[3];
    QualType cA = A.with(QualType::C);                                       // const A
    return std::tuple{t.canonical(cA), arr_i};
  };
  TypeTable t1, t2;
  auto [c_a1, arr1] = types(t1, false);
  auto [c_a2, arr2] = types(t2, true);
  CHECK(c_a1 == arr1);                                   // const A == const int[3]
  CHECK(c_a2 == arr2);
  CHECK(t1.fp(c_a1) == t2.fp(c_a2));                     // same fingerprint in both tables

  // 4. Dispatch over 240 kinds through the generated table, against a direct switch on 6 kinds.
  {
    using namespace bench;
    ManyInterner m;
    std::vector<NodeId> ids;
    for (u32 i = 0; i < 4096; ++i) {
      [&]<u32... I>(std::integer_sequence<u32, I...>) {
        ((i % NK == I ? (void)ids.push_back(m.intern(K<I>{i, {NodeId::none, 0}}, 1)) : (void)0), ...);
      }(std::make_integer_sequence<u32, NK>{});
    }
    u64 sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 5000; ++rep)
      for (NodeId n : ids) m.visit(n, [&]<class T>(const T& v) { sink += v.a; });
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (5000.0 * ids.size());
    std::printf("dispatch over %u kinds: %.2f ns/visit (sink %llu)\n", NK, ns, (unsigned long long)sink);
  }

  std::printf("%s (%d failures)\n", failures ? "FAILED" : "ok", failures);
  return failures != 0;
}
