// A frame's trees under construction (docs/design/ast.md §9.3). The parser and sema build nodes
// here, in the frame's arena, and the tree is interned bottom-up when the frame commits. Only
// what the committed value reaches is interned, so backtracking parses, failed deductions and
// `requires` checks leave nothing in the interners, and a cancelled frame leaks nothing.
//
// Handles made here are drafts: a draft `NodeId` carries its kind and its position among the
// builder's entries, and a draft `SeqId<T>`, for a `T` that holds nodes, its position. Lists of
// records that hold no nodes are keys and are interned eagerly (§3.6), so their `SeqId`s are
// already global. An interned node or list is brought in with `adopt`.
export module ycxx.ast.builder;

import std;
import ycxx.ast.handles;
import ycxx.ast.schema;
import ycxx.ast.nodes;
import ycxx.ast.records;
import ycxx.ast.interner;

export namespace ycxx::ast {

class Builder {
public:
  // `arena` is the frame's arena (the engine's, #14); it holds drafts until the builder is gone.
  explicit Builder(Interners& ins, std::pmr::memory_resource* arena = std::pmr::get_default_resource())
      : ins_(&ins), bytes_(arena), entries_(arena), edges_(arena) {}

  // A draft node. Its children are drafts made earlier by this builder (or adopted).
  template <class T>
    requires is_node_kind<T>
  NodeId add(const T& v, u32 width) {
    check_refs(v);
    u32 i = push_entry({.what = Nodes::index_of<T>(), .seq = false, .offset = store(v), .count = width});
    return make_node(Nodes::index_of<T>(), i);
  }

  // A draft standing for a node already interned.
  NodeId adopt(NodeId global)
    pre(ins_->ast.contains(global))
  {
    u32 i = push_entry({.what = kind_of(global), .adopted = true, .global = std::to_underlying(global)});
    return make_node(kind_of(global), i);
  }

  // The interned node this draft adopted, or none for a node built in this frame.
  NodeId adopted_node(NodeId draft) const
    pre(is_draft_node(draft))
  {
    const Entry& e = entry(draft);
    return e.adopted ? NodeId(e.global) : NodeId::none;
  }

  // Children for a `ChildList`, in this builder's edge pool.
  ChildList list(std::span<const Child> cs) {
    for (const Child& c : cs) check_node(c.id);
    ChildList l{static_cast<u32>(edges_.size()), static_cast<u32>(cs.size())};
    edges_.insert(edges_.end(), cs.begin(), cs.end());
    return l;
  }
  ChildList list(std::initializer_list<Child> cs) { return list(std::span<const Child>(cs.begin(), cs.size())); }

  // A list: a draft if its elements hold nodes, else interned at once (§3.6).
  template <class T>
    requires is_seq_element<T>
  SeqId<T> seq(std::span<const T> xs) {
    if constexpr (!holds_nodes_v<T>) {
      return ins_->intern_seq(xs);
    } else {
      for (const T& x : xs) check_refs(x);
      u32 offset = static_cast<u32>(bytes_.size());
      for (const T& x : xs) store(x);
      u32 i = push_entry({.what = SeqElements::index_of<T>(), .seq = true, .offset = offset,
                          .count = static_cast<u32>(xs.size())});
      return SeqId<T>{i};
    }
  }
  template <class T>
    requires is_seq_element<T>
  SeqId<T> seq(std::initializer_list<T> xs) { return seq(std::span<const T>(xs.begin(), xs.size())); }

  template <class T>
    requires is_seq_element<T> && holds_nodes_v<T>
  SeqId<T> adopt(SeqId<T> global)
    pre(ins_->seqs.contains(global))
  {
    return SeqId<T>{push_entry({.what = SeqElements::index_of<T>(), .seq = true, .adopted = true, .global = global.index})};
  }

  // Reading drafts back, as a parser does while it builds.
  template <class T>
    requires is_node_kind<T>
  T get(NodeId draft) const
    pre(is_draft_node(draft) && !entry(draft).adopted && kind_of(draft) == Nodes::index_of<T>())
  {
    return load<T>(entry(draft).offset);
  }
  u32 width(NodeId draft) const
    pre(is_draft_node(draft) && !entry(draft).adopted)
  {
    return entry(draft).count;
  }
  u32 draft_count() const { return static_cast<u32>(entries_.size()); }

  // Interns what `root` reaches, children first, and returns `root` with every draft handle
  // replaced by its interned one. Drafts it does not reach are never interned. A builder can
  // commit more than once; a draft is interned at most once.
  template <class V>
  V commit(const V& root) {
    check_refs(root);
    std::vector<bool> reached(entries_.size());
    std::vector<u32> work;
    auto reach = [&](u32 i) {
      if (!reached[i]) {
        reached[i] = true;
        work.push_back(i);
      }
    };
    auto reach_refs = [&](const auto& v) {
      for_each_ref(v, [&]<class H>(const H& h) {
        if constexpr (std::is_same_v<H, NodeId>) {
          if (h != NodeId::none) reach(index_of(h));
        } else if constexpr (std::is_same_v<H, ChildList>) {
          for (const Child& c : std::span(edges_).subspan(h.offset, h.size))
            if (c.id != NodeId::none) reach(index_of(c.id));
        } else if constexpr (is_seq_id<H>) {
          if constexpr (holds_nodes_v<typename seq_element<H>::type>) reach(h.index);
        }
      });
    };
    reach_refs(root);
    while (!work.empty()) {
      u32 i = work.back();
      work.pop_back();
      const Entry& e = entries_[i];
      if (e.adopted || e.committed) continue;
      if (e.seq) {
        dispatch<SeqElements>(e.what, [&]<class T>(std::type_identity<T>) {
          for (u32 k = 0; k < e.count; ++k) reach_refs(load<T>(e.offset + k * u32(sizeof(T))));
        });
      } else {
        dispatch<Nodes>(e.what, [&]<class T>(std::type_identity<T>) { reach_refs(load<T>(e.offset)); });
      }
    }
    // Entries refer only to earlier entries, so index order is bottom-up.
    for (u32 i = 0; i < entries_.size(); ++i) {
      Entry& e = entries_[i];
      if (!reached[i] || e.committed) continue;
      if (!e.adopted) e.global = e.seq ? intern_seq_entry(e) : intern_node_entry(e);
      e.committed = true;
    }
    V r = root;
    for_each_ref(r, [&]<class H>(H& h) { h = to_global(h); });
    return r;
  }

private:
  struct Entry {
    u32 what = 0;  // node kind, or element type in SeqElements
    bool seq = false;
    bool adopted = false;
    bool committed = false;
    u32 offset = 0;  // into bytes_
    u32 count = 0;   // a node's width, a list's length
    u32 global = 0;  // the interned handle's bits, once adopted or committed
  };

  template <class T>
  struct seq_element;
  template <class T>
  struct seq_element<SeqId<T>> {
    using type = T;
  };

  u32 push_entry(Entry e)
    pre(entries_.size() < max_node_index)
  {
    entries_.push_back(e);
    return static_cast<u32>(entries_.size() - 1);
  }

  template <class T>
  u32 store(const T& v) {
    u32 offset = static_cast<u32>(bytes_.size());
    bytes_.resize(bytes_.size() + sizeof(T));
    std::memcpy(bytes_.data() + offset, &v, sizeof(T));
    return offset;
  }
  template <class T>
  T load(u32 offset) const {
    T v;
    std::memcpy(&v, bytes_.data() + offset, sizeof(T));
    return v;
  }

  bool is_draft_node(NodeId n) const {
    return n != NodeId::none && index_of(n) < entries_.size() && !entries_[index_of(n)].seq &&
           entries_[index_of(n)].what == kind_of(n);
  }
  const Entry& entry(NodeId draft) const { return entries_[index_of(draft)]; }

  void check_node(NodeId n) const { contract_assert(n == NodeId::none || is_draft_node(n)); }

  // A value may refer only to this builder's drafts (and to global lists that hold no nodes).
  template <class V>
  void check_refs(const V& v) const {
    for_each_ref(v, [this]<class H>(const H& h) {
      if constexpr (std::is_same_v<H, NodeId>) {
        check_node(h);
      } else if constexpr (std::is_same_v<H, ChildList>) {
        bool in_edges = h.offset + h.size <= edges_.size();  // BOOTSTRAP: contracts: names a member
        contract_assert(in_edges);
      } else if constexpr (is_seq_id<H>) {
        using T = typename seq_element<H>::type;
        if constexpr (holds_nodes_v<T>) {
          // BOOTSTRAP: contracts: names a member, as in Interners::intern.
          bool draft = h.index < entries_.size() && entries_[h.index].seq &&
                       entries_[h.index].what == SeqElements::index_of<T>();
          contract_assert(draft);
        }
      }
    });
  }

  template <class H>
  H to_global(H h) const {
    if constexpr (std::is_same_v<H, NodeId>) {
      if (h == NodeId::none) return h;
      contract_assert(entries_[index_of(h)].committed);
      return NodeId(entries_[index_of(h)].global);
    } else if constexpr (is_seq_id<H>) {
      if constexpr (holds_nodes_v<typename seq_element<H>::type>) {
        contract_assert(entries_[h.index].committed);
        return H{entries_[h.index].global};
      } else {
        return h;
      }
    } else {
      return h;  // other interners' handles are global already
    }
  }

  u32 intern_node_entry(const Entry& e) {
    return dispatch<Nodes>(e.what, [&]<class T>(std::type_identity<T>) {
      T v = load<T>(e.offset);
      std::vector<Child> edges;
      for_each_ref(v, [&]<class H>(H& h) {
        if constexpr (std::is_same_v<H, ChildList>) {
          ChildList l{static_cast<u32>(edges.size()), h.size};
          for (Child c : std::span(edges_).subspan(h.offset, h.size)) edges.push_back({to_global(c.id), c.at});
          h = l;
        } else {
          h = to_global(h);
        }
      });
      return std::to_underlying(ins_->intern(v, e.count, edges));
    });
  }

  u32 intern_seq_entry(const Entry& e) {
    return dispatch<SeqElements>(e.what, [&]<class T>(std::type_identity<T>) {
      std::vector<T> xs;
      for (u32 k = 0; k < e.count; ++k) {
        T x = load<T>(e.offset + k * u32(sizeof(T)));
        for_each_ref(x, [&]<class H>(H& h) { h = to_global(h); });
        xs.push_back(x);
      }
      return ins_->intern_seq(std::span<const T>(xs)).index;
    });
  }

  Interners* ins_;
  std::pmr::vector<std::byte> bytes_;
  std::pmr::vector<Entry> entries_;
  std::pmr::vector<Child> edges_;
};

}  // namespace ycxx::ast
