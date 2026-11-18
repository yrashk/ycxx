// The interners of docs/design/ast.md §9: one per handle type, process-wide, append-only during a
// revision and compacted at quiescence (ycxx.ast.compact). Identity is the 128-bit Merkle
// fingerprint (§9.2); equal content interns to one handle, and a fingerprint does not depend on
// interning order (§2, invariant 3).
//
// Per-kind storage is an array of untyped pools indexed by kind number, never a `std::tuple` over
// the kinds (§9.4, §10.6). Fingerprint, structural equality and handle walking are written once,
// generically, by reflection over the node kinds and records.
//
// Single-threaded for now: the engine (#14) decides how interners are shared between workers.
export module ycxx.ast.interner;

import std;
import ycxx.ast.fingerprint;
import ycxx.ast.handles;
import ycxx.ast.schema;
import ycxx.ast.nodes;
import ycxx.ast.records;

export namespace ycxx::ast {

// ---------------------------------------------------------------- dispatch on a kind number

// Calls `f(std::type_identity<T>{})` for the `i`-th type of `List`, through a table of function
// pointers generated from the list (§10.5).
template <class List, class F>
decltype(auto) dispatch(u32 i, F&& f)
  pre(i < List::size)
{
  using R = decltype(f(std::type_identity<typename List::template at<0>>{}));
  using Fn = R (*)(F&);
  static constexpr auto table = []<u32... I>(std::integer_sequence<u32, I...>) {
    return std::array<Fn, List::size>{
        +[](F& g) -> R { return g(std::type_identity<typename List::template at<I>>{}); }...};
  }(std::make_integer_sequence<u32, List::size>{});
  return table[i](f);
}

// ---------------------------------------------------------------- walking

// Calls `f` on every reference a value holds: each handle (a `NodeId`, `SeqId<T>` or any other
// member of `handles`, including those inside `Child` and `DeclId`) and each `ChildList`. It does
// not descend into the nodes or lists referred to. `V` may be const; `f` gets references, so it
// can rewrite them (compaction, frame commit).
template <class V, class F>
constexpr void for_each_ref(V& v, F&& f) {
  using T = std::remove_const_t<V>;
  if constexpr (is_handle<T> || std::is_same_v<T, ChildList>) {
    f(v);
  } else if constexpr (std::is_same_v<T, Child>) {
    f(v.id);
  } else if constexpr (std::is_class_v<T>) {
    template for (constexpr auto m : members_of<T>()) for_each_ref(v.[:m:], f);
  }
}

constexpr bool is_none(auto h) {
  if constexpr (requires { std::to_underlying(h); }) return std::to_underlying(h) == 0xffffffffu;
  else return false;
}

inline constexpr Fp128 none_fp = seed("ycxx.ast.none");

// ---------------------------------------------------------------- untyped pools

// A fixed-stride pool of trivially copyable values, copied in and out.
class RawPool {
public:
  RawPool() = default;
  explicit RawPool(u32 stride) : stride_(stride) {}

  u32 size() const { return stride_ == 0 ? 0 : static_cast<u32>(bytes_.size() / stride_); }
  u32 stride() const { return stride_; }

  template <class T>
  T load(u32 i) const
    pre(sizeof(T) == stride_)
    pre(i < size())
  {
    T v;
    std::memcpy(&v, bytes_.data() + std::size_t(i) * stride_, sizeof(T));
    return v;
  }

  template <class T>
  u32 push(const T& v)
    pre(sizeof(T) == stride_)
  {
    u32 i = size();
    bytes_.resize(bytes_.size() + sizeof(T));
    std::memcpy(bytes_.data() + std::size_t(i) * stride_, &v, sizeof(T));
    return i;
  }

private:
  std::vector<std::byte> bytes_;
  u32 stride_ = 0;
};

template <class List>
std::array<RawPool, List::size> make_pools() {
  return []<u32... I>(std::integer_sequence<u32, I...>) {
    return std::array<RawPool, List::size>{RawPool(sizeof(typename List::template at<I>))...};
  }(std::make_integer_sequence<u32, List::size>{});
}

// ---------------------------------------------------------------- names

class Names {
public:
  NameId intern(std::string_view spelling) {
    Fp128 f = mix_bytes(seed("ycxx.ast.NameId"), spelling);
    if (auto it = table_.find(f); it != table_.end()) {
      contract_assert(spellings_[std::to_underlying(it->second)] == spelling);
      return it->second;
    }
    NameId n{static_cast<u32>(spellings_.size())};
    spellings_.emplace_back(spelling);
    fps_.push_back(f);
    table_.emplace(f, n);
    return n;
  }
  std::string_view spelling(NameId n) const
    pre(std::to_underlying(n) < spellings_.size())
  {
    return spellings_[std::to_underlying(n)];
  }
  Fp128 fp(NameId n) const
    pre(std::to_underlying(n) < fps_.size())
  {
    return fps_[std::to_underlying(n)];
  }
  u32 size() const { return static_cast<u32>(fps_.size()); }

private:
  std::deque<std::string> spellings_;
  std::vector<Fp128> fps_;
  std::unordered_map<Fp128, NameId, Fp128Hash> table_;
};

// ---------------------------------------------------------------- placeholder interners

// A stand-in for the interners other issues own: tokens and items (#18), entities and types and
// values (#33). It records a fingerprint per handle and nothing else, so trees can refer to these
// handles and be fingerprinted before their owners exist. Each owner replaces its member of
// `Interners` with the real interner, which keeps this `fp` signature.
class FpTable {
public:
  // The handle of the value with fingerprint `f`; equal fingerprints give equal handles.
  template <class H>
  H add(Fp128 f) {
    auto [it, fresh] = table_.emplace(f, static_cast<u32>(fps_.size()));
    if (fresh) fps_.push_back(f);
    if constexpr (std::is_same_v<H, QualType>) return make_qual_type(it->second);
    else return H{it->second};
  }
  template <class H>
  Fp128 fp(H h) const {
    if (is_none(h)) return none_fp;
    if constexpr (std::is_same_v<H, QualType>) {
      contract_assert(type_node_of(h) < fps_.size());
      return mix(fps_[type_node_of(h)], std::to_underlying(cv_of(h)));
    } else {
      contract_assert(std::to_underlying(h) < fps_.size());
      return fps_[std::to_underlying(h)];
    }
  }

private:
  std::vector<Fp128> fps_;
  std::unordered_map<Fp128, u32, Fp128Hash> table_;
};

// ---------------------------------------------------------------- tree nodes

// The `ast` interner: per kind, a payload pool and parallel arrays of fingerprints and widths;
// one edge pool; a table from fingerprint to node. The kind is in the handle, so nodes have no
// header (§9.4). This is the storage; `Interners::intern` is how nodes get in.
class Ast {
public:
  Ast() : pools_(make_pools<Nodes>()) {}

  u32 size(u32 kind) const { return pools_[kind].size(); }
  u32 node_count() const {
    u32 n = 0;
    for (const auto& p : pools_) n += p.size();
    return n;
  }
  u32 edge_count() const { return static_cast<u32>(edges_.size()); }

  bool contains(NodeId n) const { return kind_of(n) < Nodes::size && index_of(n) < size(kind_of(n)); }

  template <class T>
    requires is_node_kind<T>
  T get(NodeId n) const
    pre(kind_of(n) == Nodes::index_of<T>())
  {
    return pools_[kind_of(n)].template load<T>(index_of(n));
  }

  template <class T>
    requires is_node_kind<T>
  bool is(NodeId n) const { return n != NodeId::none && kind_of(n) == Nodes::index_of<T>(); }

  Fp128 fp(NodeId n) const
    pre(n == NodeId::none || contains(n))
  {
    return n == NodeId::none ? none_fp : fps_[kind_of(n)][index_of(n)];
  }

  // The node's extent in tokens (§3.4).
  u32 width(NodeId n) const
    pre(contains(n))
  {
    return widths_[kind_of(n)][index_of(n)];
  }

  std::span<const Child> edges() const { return edges_; }
  std::span<const Child> children(ChildList l) const
    pre(l.offset + l.size <= edges_.size())
  {
    return std::span(edges_).subspan(l.offset, l.size);
  }

  // Low level, for the interning and compaction code: look up a fingerprint, append a node whose
  // lists are already in this pool, append edges.
  std::optional<NodeId> find(Fp128 f) const {
    if (auto it = table_.find(f); it != table_.end()) return it->second;
    return std::nullopt;
  }
  template <class T>
    requires is_node_kind<T>
  NodeId insert(const T& v, u32 width, Fp128 f)
    pre(!find(f))
  {
    constexpr u32 k = Nodes::index_of<T>();
    NodeId n = make_node(k, pools_[k].push(v));
    fps_[k].push_back(f);
    widths_[k].push_back(width);
    table_.emplace(f, n);
    return n;
  }
  ChildList append_edges(std::span<const Child> cs) {
    ChildList l{static_cast<u32>(edges_.size()), static_cast<u32>(cs.size())};
    edges_.insert(edges_.end(), cs.begin(), cs.end());
    return l;
  }

private:
  std::array<RawPool, Nodes::size> pools_;
  std::array<std::vector<Fp128>, Nodes::size> fps_;
  std::array<std::vector<u32>, Nodes::size> widths_;
  std::vector<Child> edges_;
  std::unordered_map<Fp128, NodeId, Fp128Hash> table_;
};

// ---------------------------------------------------------------- interned lists

template <class T>
constexpr bool is_seq_element = SeqElements::contains<T>;

// The `seqs` interner: per element type, a pool of elements and a table of ranges into it.
class Seqs {
public:
  Seqs() : elements_(make_pools<SeqElements>()) {}

  template <class T>
    requires is_seq_element<T>
  u32 size(SeqId<T> s) const { return range(s).size; }

  template <class T>
    requires is_seq_element<T>
  T at(SeqId<T> s, u32 i) const
    pre(i < size(s))
  {
    return elements_[SeqElements::index_of<T>()].template load<T>(range(s).offset + i);
  }

  template <class T>
    requires is_seq_element<T>
  std::vector<T> elements(SeqId<T> s) const {
    std::vector<T> r;
    r.reserve(size(s));
    for (u32 i = 0; i < size(s); ++i) r.push_back(at(s, i));
    return r;
  }

  template <class T>
    requires is_seq_element<T>
  Fp128 fp(SeqId<T> s) const { return fps_[SeqElements::index_of<T>()].at(s.index); }

  template <class T>
    requires is_seq_element<T>
  bool contains(SeqId<T> s) const { return s.index < ranges_[SeqElements::index_of<T>()].size(); }

  template <class T>
    requires is_seq_element<T>
  u32 count() const { return static_cast<u32>(ranges_[SeqElements::index_of<T>()].size()); }

  template <class T>
    requires is_seq_element<T>
  std::optional<SeqId<T>> find(Fp128 f) const {
    auto& t = tables_[SeqElements::index_of<T>()];
    if (auto it = t.find(f); it != t.end()) return SeqId<T>{it->second};
    return std::nullopt;
  }

  template <class T>
    requires is_seq_element<T>
  SeqId<T> insert(std::span<const T> xs, Fp128 f)
    pre(!find<T>(f))
  {
    constexpr u32 k = SeqElements::index_of<T>();
    Range r{elements_[k].size(), static_cast<u32>(xs.size())};
    for (const T& x : xs) elements_[k].push(x);
    SeqId<T> s{static_cast<u32>(ranges_[k].size())};
    ranges_[k].push_back(r);
    fps_[k].push_back(f);
    tables_[k].emplace(f, s.index);
    return s;
  }

private:
  struct Range {
    u32 offset;
    u32 size;
  };
  template <class T>
  Range range(SeqId<T> s) const
    pre(contains(s))
  {
    return ranges_[SeqElements::index_of<T>()][s.index];
  }

  std::array<RawPool, SeqElements::size> elements_;
  std::array<std::vector<Range>, SeqElements::size> ranges_;
  std::array<std::vector<Fp128>, SeqElements::size> fps_;
  std::array<std::unordered_map<Fp128, u32, Fp128Hash>, SeqElements::size> tables_;
};

// ---------------------------------------------------------------- the interner set

// The process-wide interners (§9.1). A member's name is the `interner` enumerator that handle
// enums name in their annotation; `fp(h)` finds the member by reflection.
struct Interners {
  Names names;
  FpTable tokens;    // placeholder until #18
  FpTable items;     // placeholder until #18
  FpTable entities;  // placeholder until #33
  Ast ast;
  FpTable types;   // placeholder until #33
  FpTable values;  // placeholder until #33
  Seqs seqs;

  // The fingerprint of any handle: Merkle, through the interner that holds it.
  template <class H>
    requires is_handle<H>
  Fp128 fp(H h) const {
    constexpr auto m = member_for(^^H);
    return this->[:m:].fp(h);
  }

  // The fingerprint of a value: its schema seed and each member in order, handles by their own
  // fingerprints. `edges` is the pool the value's `ChildList`s index.
  template <class V>
  Fp128 fingerprint(const V& v, std::span<const Child> edges) const {
    return fp_value(Fp128{}, v, edges);
  }

  // Interns a node. Its children must be interned; its `ChildList`s index `edges`, and are copied
  // into the edge pool if the node is new. Equal content and width give the same handle.
  template <class T>
    requires is_node_kind<T>
  NodeId intern(const T& v, u32 width, std::span<const Child> edges = {}) {
    // BOOTSTRAP: contracts: a predicate inside a lambda is evaluated into a local first; in a
    // lambda's contract predicate GCC 16.2 takes `this` to be the closure, so `ast` read garbage
    // (STATUS).
    for_each_ref(v, [this, edges]<class H>(const H& h) {
      if constexpr (std::is_same_v<H, NodeId>) {
        bool interned = h == NodeId::none || ast.contains(h);
        contract_assert(interned);
      } else if constexpr (std::is_same_v<H, ChildList>) {
        bool in_edges = h.offset + h.size <= edges.size();
        contract_assert(in_edges);
      }
    });
    Fp128 f = mix(fingerprint(v, edges), width);
    if (auto hit = ast.find(f)) {
      // Fingerprints are identity (§9.2); a collision is a bug, caught here.
      contract_assert(ast.is<T>(*hit) && ast.width(*hit) == width && same(ast.get<T>(*hit), ast.edges(), v, edges));
      return *hit;
    }
    T stored = v;
    for_each_ref(stored, [&]<class H>(H& h) {
      if constexpr (std::is_same_v<H, ChildList>) h = ast.append_edges(edges.subspan(h.offset, h.size));
    });
    return ast.insert(stored, width, f);
  }

  // Interns a list. Its elements' handles must be interned.
  template <class T>
    requires is_seq_element<T>
  SeqId<T> intern_seq(std::span<const T> xs) {
    Fp128 f = mix(schema_seed_v<SeqId<T>>, xs.size());
    for (const T& x : xs) f = fp_value(f, x, {});
    if (auto hit = seqs.find<T>(f)) {
      contract_assert(std::ranges::equal(seqs.elements(*hit), xs));
      return *hit;
    }
    return seqs.insert<T>(xs, f);
  }
  template <class T>
    requires is_seq_element<T>
  SeqId<T> intern_seq(std::initializer_list<T> xs) {
    return intern_seq(std::span<const T>(xs.begin(), xs.size()));
  }

  // Structural equality of two values whose lists index `ea` and `eb`. Handles compare by value:
  // children are interned first, so equal subtrees are equal handles.
  template <class V>
  static bool same(const V& a, std::span<const Child> ea, const V& b, std::span<const Child> eb) {
    if constexpr (std::is_same_v<V, ChildList>) {
      return a.size == b.size && std::ranges::equal(ea.subspan(a.offset, a.size), eb.subspan(b.offset, b.size));
    } else if constexpr (std::is_class_v<V> && !is_handle<V>) {
      bool r = true;
      template for (constexpr auto m : members_of<V>()) r = r && same(a.[:m:], ea, b.[:m:], eb);
      return r;
    } else {
      return a == b;
    }
  }

private:
  static consteval std::meta::info member_for(std::meta::info h) {
    bool seq = std::meta::has_template_arguments(h) && std::meta::template_of(h) == ^^SeqId;
    interner which = seq ? interner::seqs : interner_of(h);
    for (auto e : std::meta::enumerators_of(^^interner))
      if (std::meta::extract<interner>(e) == which)
        for (auto m : std::meta::nonstatic_data_members_of(^^Interners, std::meta::access_context::unchecked()))
          if (std::meta::identifier_of(m) == std::meta::identifier_of(e)) return m;
    throw std::meta::exception(u8"no member of Interners for this handle's interner", h);
  }

  template <class V>
  Fp128 fp_value(Fp128 h, const V& v, std::span<const Child> edges) const {
    if constexpr (is_handle<V>) {
      return mix(h, fp(v));
    } else if constexpr (std::is_same_v<V, ChildList>) {
      h = mix(h, v.size);
      for (const Child& c : edges.subspan(v.offset, v.size)) h = fp_value(h, c, edges);
      return h;
    } else if constexpr (std::is_enum_v<V> || std::is_integral_v<V>) {
      return mix(h, static_cast<u64>(v));
    } else {
      Fp128 s = schema_seed_v<V>;
      template for (constexpr auto m : members_of<V>()) s = fp_value(s, v.[:m:], edges);
      return mix(h, s);
    }
  }
};

// Every interner the handles name is a member of `Interners`.
static_assert([] {
  for (auto e : std::meta::enumerators_of(^^interner)) {
    bool found = false;
    for (auto m : std::meta::nonstatic_data_members_of(^^Interners, std::meta::access_context::unchecked()))
      found = found || std::meta::identifier_of(m) == std::meta::identifier_of(e);
    if (!found) return false;
  }
  return true;
}());

}  // namespace ycxx::ast
