// The interners of docs/design/ast.md §9: one per handle type, process-wide, append-only during a
// revision and compacted at quiescence (ycxx.ast.compact). Identity is the 128-bit Merkle
// fingerprint (§9.2); equal content interns to one handle, and a fingerprint does not depend on
// interning order (§2, invariant 3).
//
// Per-kind storage is an array of untyped pools indexed by kind number, never a `std::tuple` over
// the kinds (§9.4, §10.6). Fingerprint, structural equality and handle walking are written once,
// generically, by reflection over the node kinds and records.
//
// Shared by the engine's workers (#107): pools are append-only and readable without a lock, and a
// fingerprint's lookup and insertion happen under one shard lock of the interner's index
// (ycxx.ast.store), so equal content interns to one handle whichever worker comes first. A
// handle's number may differ between runs; its fingerprint never does.
export module ycxx.ast.interner;

import std;
export import ycxx.ast.store;
import ycxx.ast.fingerprint;
import ycxx.ast.handles;
import ycxx.ast.schema;
import ycxx.ast.nodes;
import ycxx.ast.records;
import ycxx.ast.types;
import ycxx.ast.values;

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

// A fixed-stride pool of trivially copyable values, copied in and out. Append-only and readable
// while other workers append (ycxx.ast.store).
class RawPool {
public:
  RawPool() = default;
  explicit RawPool(u32 stride) : units_(stride) {}

  u32 size() const { return units_.size(); }
  u32 stride() const { return units_.unit(); }

  template <class T>
  T load(u32 i) const
    pre(sizeof(T) == stride())
    pre(i < size())
  {
    T v;
    std::memcpy(&v, units_.at(i), sizeof(T));
    return v;
  }

  template <class T>
  void store(u32 i, const T& v)
    pre(sizeof(T) == stride())
    pre(i < size())
  {
    std::memcpy(units_.at(i), &v, sizeof(T));
  }

  template <class T>
  u32 push(const T& v)
    pre(sizeof(T) == stride())
  {
    const u32 i = units_.alloc(1);
    std::memcpy(units_.at(i), &v, sizeof(T));
    return i;
  }

  // `xs` as one contiguous range; its first index.
  template <class T>
  u32 append(std::span<const T> xs)
    pre(sizeof(T) == stride())
  {
    const u32 at = units_.alloc(static_cast<u32>(xs.size()));
    if (!xs.empty()) std::memcpy(units_.at(at), xs.data(), xs.size_bytes());
    return at;
  }

private:
  Chunked units_;
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
    auto [n, fresh] = table_.find_or_insert(f, [&] {
      const u32 at = chars_.append(std::span<const char>(spelling.data(), spelling.size()));
      const u32 i = ranges_.push({at, static_cast<u32>(spelling.size())});
      fps_.put(i, f);
      return NameId{i};
    });
    if (!fresh) contract_assert(this->spelling(n) == spelling);
    return n;
  }
  std::string_view spelling(NameId n) const
    pre(std::to_underlying(n) < size())
  {
    const Range r = ranges_.get(std::to_underlying(n));
    const auto cs = chars_.span(r.offset, r.size);
    return {cs.data(), cs.size()};
  }
  Fp128 fp(NameId n) const
    pre(std::to_underlying(n) < size())
  {
    return fps_.get(std::to_underlying(n));
  }
  u32 size() const { return ranges_.size(); }

private:
  struct Range {
    u32 offset;
    u32 size;
  };
  Column<char> chars_;
  Column<Range> ranges_;
  Column<Fp128> fps_;
  FpIndex<NameId> table_;
};

// ---------------------------------------------------------------- placeholder interners

// A stand-in for the interners other issues own: tokens and items (#18), entities (#33). It
// records a fingerprint per handle and nothing else, so trees can refer to these handles and be
// fingerprinted before their owners exist. Each owner replaces its member of
// `Interners` with the real interner, which keeps this `fp` signature.
class FpTable {
public:
  // The handle of the value with fingerprint `f`; equal fingerprints give equal handles.
  template <class H>
  H add(Fp128 f) {
    const u32 n = table_.find_or_insert(f, [&] { return fps_.push(f); }).first;
    if constexpr (std::is_same_v<H, QualType>) return make_qual_type(n);
    else return H{n};
  }
  template <class H>
  Fp128 fp(H h) const {
    if (is_none(h)) return none_fp;
    if constexpr (std::is_same_v<H, QualType>) {
      contract_assert(type_node_of(h) < fps_.size());
      return mix(fps_.get(type_node_of(h)), std::to_underlying(cv_of(h)));
    } else {
      contract_assert(std::to_underlying(h) < fps_.size());
      return fps_.get(std::to_underlying(h));
    }
  }

private:
  Column<Fp128> fps_;
  FpIndex<u32> table_;
};

// ---------------------------------------------------------------- types and values

// Storage for an interner whose handle has no room for a kind: `types` and `values` (§5). Values
// are numbered densely across kinds; per value, its kind, its slot in the kind's pool and its
// fingerprint. `Interners::intern_type` and `intern_value` are how values get in.
template <class List>
class KindTable {
public:
  KindTable() : pools_(make_pools<List>()) {}

  u32 size() const { return kinds_.size(); }
  bool contains(u32 n) const { return n < size(); }
  u32 kind(u32 n) const
    pre(contains(n))
  {
    return kinds_.get(n);
  }

  template <class T>
    requires(List::template contains<T>)
  bool is(u32 n) const { return contains(n) && kinds_.get(n) == List::template index_of<T>(); }

  template <class T>
    requires(List::template contains<T>)
  T get(u32 n) const
    pre(is<T>(n))
  {
    return pools_[kinds_.get(n)].template load<T>(slots_.get(n));
  }

  Fp128 fp(u32 n) const
    pre(contains(n))
  {
    return fps_.get(n);
  }

  std::optional<u32> find(Fp128 f) const { return table_.find(f); }

  // The value with fingerprint `f`, inserting `v` if there is none; whether it was inserted.
  // `also(n)` runs under the same lock for a new value, before anyone else can find it.
  template <class T, class Also = void (*)(u32)>
    requires(List::template contains<T>)
  std::pair<u32, bool> find_or_insert(const T& v, Fp128 f, Also also = [](u32) {}) {
    return table_.find_or_insert(f, [&] {
      constexpr u32 k = List::template index_of<T>();
      const u32 slot = pools_[k].push(v);
      const u32 n = kinds_.push(static_cast<u8>(k));
      slots_.put(n, slot);
      fps_.put(n, f);
      also(n);
      return n;
    });
  }

  template <class T>
    requires(List::template contains<T>)
  u32 insert(const T& v, Fp128 f)
    pre(!find(f))
  {
    return find_or_insert(v, f).first;
  }

  // Rewrites value `n` in place with content of the same fingerprint (compaction renumbers the
  // lists a value holds).
  template <class T>
    requires(List::template contains<T>)
  void replace(u32 n, const T& v)
    pre(is<T>(n))
  {
    pools_[kinds_.get(n)].store(slots_.get(n), v);
  }

private:
  std::array<RawPool, List::size> pools_;
  Column<u8> kinds_;
  Column<u32> slots_;
  Column<Fp128> fps_;
  FpIndex<u32> table_;
};

// The `types` interner (§5.1, §5.2): the kind table and `canon[]`, the canonical form of each
// node, filled when it is interned. Accessors take a `QualType` and ignore its cv.
class Types {
public:
  u32 size() const { return table_.size(); }
  bool contains(QualType q) const { return table_.contains(type_node_of(q)); }
  u32 kind(QualType q) const { return table_.kind(type_node_of(q)); }
  template <class T>
    requires is_type_kind<T>
  bool is(QualType q) const { return table_.template is<T>(type_node_of(q)); }
  template <class T>
    requires is_type_kind<T>
  T get(QualType q) const { return table_.template get<T>(type_node_of(q)); }

  Fp128 fp(QualType q) const { return mix(table_.fp(type_node_of(q)), std::to_underlying(cv_of(q))); }

  // The canonical form of `q`'s node, cv included (`typedef const int A;`). `q`'s own cv is not
  // applied; `Interners::canonical` does that.
  QualType canon(QualType q) const
    pre(contains(q))
  {
    return canon_.get(type_node_of(q));
  }

  KindTable<TypeKinds>& table() { return table_; }
  const KindTable<TypeKinds>& table() const { return table_; }

  // `canon` is the node's canonical form, or none for a canonical node, which is its own. A node
  // another worker interned first keeps its own (equal) canonical form.
  template <class T>
    requires is_type_kind<T>
  QualType find_or_insert(const T& v, Fp128 f, std::optional<QualType> canon) {
    return make_qual_type(table_.find_or_insert(v, f, [&](u32 n) { canon_.put(n, canon.value_or(make_qual_type(n))); }).first);
  }

private:
  KindTable<TypeKinds> table_;
  Column<QualType> canon_;
};

// The `values` interner (§5.5).
class Values {
public:
  u32 size() const { return table_.size(); }
  bool contains(ConstId c) const { return c == ConstId::none || table_.contains(std::to_underlying(c)); }
  u32 kind(ConstId c) const { return table_.kind(std::to_underlying(c)); }
  template <class T>
    requires is_value_kind<T>
  bool is(ConstId c) const { return c != ConstId::none && table_.template is<T>(std::to_underlying(c)); }
  template <class T>
    requires is_value_kind<T>
  T get(ConstId c) const { return table_.template get<T>(std::to_underlying(c)); }

  Fp128 fp(ConstId c) const { return c == ConstId::none ? none_fp : table_.fp(std::to_underlying(c)); }

  // Whether `c` has free template parameters: a `VDep*` value (§5.5).
  // BOOTSTRAP: contracts: a table, not `dispatch`: GCC 16.2 ICEs (gimplify_expr) on dispatch's
  // `pre` instantiated from here (STATUS).
  bool is_dependent(ConstId c) const {
    static constexpr auto dependent_kinds = []<u32... I>(std::integer_sequence<u32, I...>) {
      return std::array{is_dependent_kind<typename ValueKinds::template at<I>>...};
    }(std::make_integer_sequence<u32, ValueKinds::size>{});
    return c != ConstId::none && dependent_kinds[kind(c)];
  }

  KindTable<ValueKinds>& table() { return table_; }
  const KindTable<ValueKinds>& table() const { return table_; }

private:
  KindTable<ValueKinds> table_;
};

// ---------------------------------------------------------------- tree nodes

// The `ast` interner: per kind, a payload pool and parallel columns of fingerprints and widths;
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
  // Edge slots handed out (a block's skipped tail included).
  u32 edge_count() const { return edges_.size(); }

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
    return n == NodeId::none ? none_fp : fps_[kind_of(n)].get(index_of(n));
  }

  // The node's extent in tokens (§3.4).
  u32 width(NodeId n) const
    pre(contains(n))
  {
    return widths_[kind_of(n)].get(index_of(n));
  }

  std::span<const Child> children(ChildList l) const
    pre(l.offset + l.size <= edge_count())
  {
    return edges_.span(l.offset, l.size);
  }

  // Low level, for the interning and compaction code: look up a fingerprint, insert a node whose
  // lists `make` copies into this pool (under the fingerprint's lock), append edges.
  std::optional<NodeId> find(Fp128 f) const { return table_.find(f); }
  template <class T, class Make>
    requires is_node_kind<T>
  std::pair<NodeId, bool> find_or_insert(Fp128 f, u32 width, Make&& make) {
    return table_.find_or_insert(f, [&] {
      constexpr u32 k = Nodes::index_of<T>();
      const T stored = make();
      const NodeId n = make_node(k, pools_[k].push(stored));
      fps_[k].put(index_of(n), f);
      widths_[k].put(index_of(n), width);
      return n;
    });
  }
  template <class T>
    requires is_node_kind<T>
  NodeId insert(const T& v, u32 width, Fp128 f)
    pre(!find(f))
  {
    return find_or_insert<T>(f, width, [&] { return v; }).first;
  }
  ChildList append_edges(std::span<const Child> cs) {
    return ChildList{edges_.append(cs), static_cast<u32>(cs.size())};
  }

private:
  std::array<RawPool, Nodes::size> pools_;
  std::array<Column<Fp128>, Nodes::size> fps_;
  std::array<Column<u32>, Nodes::size> widths_;
  Column<Child> edges_;
  FpIndex<NodeId> table_;
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
  Fp128 fp(SeqId<T> s) const { return fps_[SeqElements::index_of<T>()].get(s.index); }

  template <class T>
    requires is_seq_element<T>
  bool contains(SeqId<T> s) const { return s.index < ranges_[SeqElements::index_of<T>()].size(); }

  template <class T>
    requires is_seq_element<T>
  u32 count() const { return ranges_[SeqElements::index_of<T>()].size(); }

  template <class T>
    requires is_seq_element<T>
  std::optional<SeqId<T>> find(Fp128 f) const {
    if (auto i = tables_[SeqElements::index_of<T>()].find(f)) return SeqId<T>{*i};
    return std::nullopt;
  }

  // The list with fingerprint `f`, inserting `xs` if there is none; whether it was inserted.
  template <class T>
    requires is_seq_element<T>
  std::pair<SeqId<T>, bool> find_or_insert(std::span<const T> xs, Fp128 f) {
    constexpr u32 k = SeqElements::index_of<T>();
    auto [i, fresh] = tables_[k].find_or_insert(f, [&] {
      const Range r{elements_[k].append(xs), static_cast<u32>(xs.size())};
      const u32 n = ranges_[k].push(r);
      fps_[k].put(n, f);
      return n;
    });
    return {SeqId<T>{i}, fresh};
  }

  template <class T>
    requires is_seq_element<T>
  SeqId<T> insert(std::span<const T> xs, Fp128 f)
    pre(!find<T>(f))
  {
    return find_or_insert<T>(xs, f).first;
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
    return ranges_[SeqElements::index_of<T>()].get(s.index);
  }

  std::array<RawPool, SeqElements::size> elements_;
  std::array<Column<Range>, SeqElements::size> ranges_;
  std::array<Column<Fp128>, SeqElements::size> fps_;
  std::array<FpIndex<u32>, SeqElements::size> tables_;
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
  Types types;
  Values values;
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
    return fp_value(Fp128{}, v, SpanEdges{edges});
  }

  // Interns a node. Its children must be interned; its `ChildList`s index `edges`, and are copied
  // into the edge pool if the node is new. Equal content and width give the same handle.
  template <class T>
    requires is_node_kind<T>
  NodeId intern(const T& v, u32 width, std::span<const Child> edges = {}) {
    return intern_with(v, width, SpanEdges{edges});
  }
  // Interns a node whose lists are already in the edge pool (a stored node, re-interned).
  template <class T>
    requires is_node_kind<T>
  NodeId intern_stored(const T& v, u32 width) {
    return intern_with(v, width, PoolEdges{&ast});
  }

private:
  // A node's lists, in a span the caller holds or in the edge pool.
  struct SpanEdges {
    std::span<const Child> edges;
    std::span<const Child> operator()(ChildList l) const { return edges.subspan(l.offset, l.size); }
  };
  struct PoolEdges {
    const Ast* ast;
    std::span<const Child> operator()(ChildList l) const { return ast->children(l); }
  };

  template <class T, class E>
    requires is_node_kind<T>
  NodeId intern_with(const T& v, u32 width, E edges) {
    for_each_ref(v, [this, edges]<class H>(const H& h) {
      if constexpr (std::is_same_v<H, NodeId>) {
        // BOOTSTRAP: contracts: in a lambda's contract predicate GCC 16.2 takes `this` to be the
        // closure, so a predicate naming a member goes through a local (STATUS).
        bool interned = h == NodeId::none || ast.contains(h);
        contract_assert(interned);
      }
    });
    Fp128 f = mix(fp_value(Fp128{}, v, edges), width);
    auto [n, fresh] = ast.find_or_insert<T>(f, width, [&] {
      T stored = v;
      for_each_ref(stored, [&]<class H>(H& h) {
        if constexpr (std::is_same_v<H, ChildList>) h = ast.append_edges(edges(h));
      });
      return stored;
    });
    if (!fresh) {
      // Fingerprints are identity (§9.2); a collision is a bug, caught here.
      bool same_content = ast.is<T>(n) && ast.width(n) == width && same_as_stored(ast.get<T>(n), v, edges);
      contract_assert(same_content);
    }
    return n;
  }

public:
  // ---------------------------------------------------------------- types (§5.1, §5.2)

  // Interns a type node with top-level `cv` (applied by `qualified`). Its handles must be
  // interned. A new node's canonical form is computed first, interning it if it differs: a sugar
  // node's is the canonical form of its `[[=canonical]]` member, any other node's is the node with
  // canonical children, which is the node itself when its children are canonical. This is the raw
  // constructor: ycxx.ast.typeops builds references, arrays and function types with the
  // construction-time rules of §5.4.
  template <class T>
    requires is_type_kind<T>
  QualType intern_type(const T& v, Cv cv = Cv::none) {
    for_each_ref(v, [this]<class H>(const H& h) {
      // BOOTSTRAP: contracts: in a lambda's contract predicate GCC 16.2 takes `this` to be the
      // closure, so a predicate naming a member goes through a local (STATUS).
      bool interned = true;
      if constexpr (std::is_same_v<H, QualType>) interned = types.contains(h);
      else if constexpr (std::is_same_v<H, ConstId>) interned = values.contains(h);
      else if constexpr (is_seq_id<H>) interned = seqs.contains(h);
      contract_assert(interned);
    });
    Fp128 f = fingerprint(v, {});
    QualType q;
    if (auto hit = types.table().find(f)) {
      q = make_qual_type(*hit);
      bool same_content = types.is<T>(q) && same(types.get<T>(q), {}, v, {});
      contract_assert(same_content);  // fingerprints are identity (§9.2)
    } else {
      // The canonical form interns other types, so it is computed outside the index's lock; a
      // worker that inserted the node meanwhile computed the same one.
      std::optional<QualType> c = canonical_form(v);
      q = types.find_or_insert(v, f, c);
    }
    return qualified(q, cv);
  }

  // `q` with `cv` added (§5.2): cv on a reference or function type, directly or through a typedef,
  // is dropped ([dcl.ref], [dcl.fct]); cv on an array node goes to its elements
  // ([basic.type.qualifier]). Sugar for an array keeps the cv, and the canonical form, the array
  // of cv-qualified elements, is interned now, so `canonical` stays a read. A cv-qualified
  // `QualType` is made here (or by `intern_type`), never by the raw bit operation `with_cv`.
  QualType qualified(QualType q, Cv cv) {
    if (cv == Cv::none) return q;
    QualType c = types.canon(q);
    if (types.is<TLRef>(c) || types.is<TRRef>(c) || types.is<TFunction>(c)) return q;
    if (types.is<TArray>(q)) {
      TArray a = types.get<TArray>(q);
      a.elem = qualified(a.elem, cv);
      return intern_type(a);
    }
    QualType r = with_cv(q, cv);
    if (types.is<TArray>(c)) qualified(c, cv_of(r));  // the canonical form, for `canonical`
    return r;
  }

  // The canonical form of `q`, in O(1): a read of canon[] and of the cv-qualified array that
  // `qualified` interned.
  QualType canonical(QualType q) const { return canonical_qualified(types.canon(q), cv_of(q)); }

  bool same_type(QualType a, QualType b) const { return canonical(a) == canonical(b); }

  // ---------------------------------------------------------------- values (§5.5)

  // Interns a constant. Its handles must be interned; a type in it is canonical.
  template <class T>
    requires is_value_kind<T>
  ConstId intern_value(const T& v) {
    for_each_ref(v, [this]<class H>(const H& h) {
      if constexpr (std::is_same_v<H, QualType>) {
        bool is_canonical = types.contains(h) && types.canon(h) == h;  // BOOTSTRAP: as above
        contract_assert(is_canonical);
      }
    });
    Fp128 f = fingerprint(v, {});
    auto [n, fresh] = values.table().find_or_insert(v, f);
    ConstId c{n};
    if (!fresh) {
      bool same_content = values.is<T>(c) && same(values.get<T>(c), {}, v, {});
      contract_assert(same_content);
    }
    return c;
  }

  // Interns a list. Its elements' handles must be interned.
  template <class T>
    requires is_seq_element<T>
  SeqId<T> intern_seq(std::span<const T> xs) {
    Fp128 f = mix(schema_seed_v<SeqId<T>>, xs.size());
    for (const T& x : xs) f = fp_value(f, x, SpanEdges{});
    auto [s, fresh] = seqs.find_or_insert<T>(xs, f);
    if (!fresh) contract_assert(std::ranges::equal(seqs.elements(s), xs));
    return s;
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
  // `same` against a stored node, whose lists are in the edge pool.
  template <class V, class E>
  bool same_as_stored(const V& stored, const V& v, E ev) const {
    if constexpr (std::is_same_v<V, ChildList>) {
      return stored.size == v.size && std::ranges::equal(ast.children(stored), ev(v));
    } else if constexpr (std::is_class_v<V> && !is_handle<V>) {
      bool r = true;
      template for (constexpr auto m : members_of<V>()) r = r && same_as_stored(stored.[:m:], v.[:m:], ev);
      return r;
    } else {
      return stored == v;
    }
  }

  // `qualified(c, cv)` for a canonical `c`, which construction has already interned.
  QualType canonical_qualified(QualType c, Cv cv) const {
    if (cv == Cv::none) return c;
    if (types.is<TLRef>(c) || types.is<TRRef>(c) || types.is<TFunction>(c)) return c;
    if (!types.is<TArray>(c)) return with_cv(c, cv);
    TArray a = types.get<TArray>(c);
    a.elem = canonical_qualified(a.elem, cv);
    auto hit = types.table().find(fingerprint(a, {}));
    contract_assert(hit.has_value());  // interned by `qualified` (§5.2)
    return make_qual_type(*hit);
  }

  // The canonical form of a node about to be interned, or none if it is its own (§5.2).
  template <class T>
  std::optional<QualType> canonical_form(const T& v) {
    if constexpr (is_sugar_kind<T>) {
      constexpr auto m = canonical_member(^^T);
      return canonical(v.[:m:]);
    } else {
      T c = v;
      for_each_ref(c, [this]<class H>(H& h) {
        if constexpr (std::is_same_v<H, QualType>) {
          h = canonical(h);
        } else if constexpr (std::is_same_v<H, SeqId<QualType>>) {
          // Parameter types have no top-level cv ([dcl.fct]); a typedef can bring it back.
          std::vector<QualType> xs = seqs.elements(h);
          for (QualType& x : xs) x = std::is_same_v<T, TFunction> ? without_cv(canonical(x)) : canonical(x);
          h = intern_seq(std::span<const QualType>(xs));
        }
      });
      if (same(c, {}, v, {})) return std::nullopt;
      return intern_type(c);
    }
  }

  static consteval std::meta::info member_for(std::meta::info h) {
    bool seq = std::meta::has_template_arguments(h) && std::meta::template_of(h) == ^^SeqId;
    interner which = seq ? interner::seqs : interner_of(h);
    for (auto e : std::meta::enumerators_of(^^interner))
      if (std::meta::extract<interner>(e) == which)
        for (auto m : std::meta::nonstatic_data_members_of(^^Interners, std::meta::access_context::unchecked()))
          if (std::meta::identifier_of(m) == std::meta::identifier_of(e)) return m;
    throw std::meta::exception(u8"no member of Interners for this handle's interner", h);
  }

  template <class V, class E>
  Fp128 fp_value(Fp128 h, const V& v, E edges) const {
    if constexpr (is_handle<V>) {
      return mix(h, fp(v));
    } else if constexpr (std::is_same_v<V, ChildList>) {
      h = mix(h, v.size);
      for (const Child& c : edges(v)) h = fp_value(h, c, edges);
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
