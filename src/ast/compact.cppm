// Compaction of the `ast` and `seqs` interners at quiescence (docs/design/ast.md §9.5, the
// query engine's §A6.2 step 3). The holders of live values mark them; everything they reach,
// through nodes, edges and lists, survives and is renumbered; the holders then rewrite their
// values with the returned map. Marking and rewriting are the one reflection walk
// (`for_each_ref`), since every reference is a handle from `handles` or a `SeqId`. Fingerprints
// do not change, so cells, records and trie leaves keyed by them are untouched; the hash tables
// are rebuilt from the fingerprint arrays.
export module ycxx.ast.compact;

import std;
import ycxx.ast.fingerprint;
import ycxx.ast.handles;
import ycxx.ast.schema;
import ycxx.ast.nodes;
import ycxx.ast.records;
import ycxx.ast.types;
import ycxx.ast.values;
import ycxx.ast.interner;

export namespace ycxx::ast {

// Old handle to new, for the nodes and lists that survived.
class Remap {
public:
  static constexpr u32 dead = 0xffffffff;

  NodeId map(NodeId n) const {
    if (n == NodeId::none) return n;
    u32 i = nodes_[kind_of(n)].at(index_of(n));
    contract_assert(i != dead);  // a holder rewrites only what it marked
    return make_node(kind_of(n), i);
  }
  template <class T>
    requires is_seq_element<T>
  SeqId<T> map(SeqId<T> s) const {
    u32 i = seqs_[SeqElements::index_of<T>()].at(s.index);
    contract_assert(i != dead);
    return SeqId<T>{i};
  }

  // Rewrites every node and list handle a value holds. Other interners' handles are untouched.
  template <class V>
  void rewrite(V& v) const {
    for_each_ref(v, [this]<class H>(H& h) {
      if constexpr (std::is_same_v<H, NodeId> || is_seq_id<H>) h = map(h);
      else if constexpr (std::is_same_v<H, ChildList>) contract_assert(false);  // only nodes hold edges
    });
  }
  template <class V>
  V operator()(V v) const {
    rewrite(v);
    return v;
  }

  bool live(NodeId n) const { return n == NodeId::none || nodes_[kind_of(n)].at(index_of(n)) != dead; }
  template <class T>
  bool live(SeqId<T> s) const { return seqs_[SeqElements::index_of<T>()].at(s.index) != dead; }

private:
  friend class Compactor;
  std::array<std::vector<u32>, Nodes::size> nodes_;
  std::array<std::vector<u32>, SeqElements::size> seqs_;
};

// What `compact` hands the holders: `mark(value)` keeps everything `value` reaches.
class Compactor {
public:
  explicit Compactor(const Interners& ins) : ins_(&ins) {
    for (u32 k = 0; k < Nodes::size; ++k) remap_.nodes_[k].assign(ins.ast.size(k), Remap::dead);
    dispatch_each<SeqElements>([&]<class T>(std::type_identity<T>) {
      remap_.seqs_[SeqElements::index_of<T>()].assign(ins.seqs.count<T>(), Remap::dead);
    });
  }

  // Types and constants are never evicted (§9.6), so the lists they hold are roots.
  void mark_interned() {
    for (u32 n = 0; n < ins_->types.size(); ++n)
      dispatch<TypeKinds>(ins_->types.table().kind(n), [&]<class T>(std::type_identity<T>) {
        mark(ins_->types.table().get<T>(n));
      });
    for (u32 n = 0; n < ins_->values.size(); ++n)
      dispatch<ValueKinds>(ins_->values.table().kind(n), [&]<class T>(std::type_identity<T>) {
        mark(ins_->values.table().get<T>(n));
      });
  }

  template <class V>
  void mark(const V& v) {
    mark_refs(v);
    while (!nodes_.empty() || !seqs_.empty()) {
      if (!nodes_.empty()) {
        NodeId n = nodes_.back();
        nodes_.pop_back();
        dispatch<Nodes>(kind_of(n), [&]<class T>(std::type_identity<T>) { mark_refs(ins_->ast.get<T>(n)); });
      } else {
        auto [k, i] = seqs_.back();
        seqs_.pop_back();
        dispatch<SeqElements>(k, [&]<class T>(std::type_identity<T>) {
          for (const T& x : ins_->seqs.elements(SeqId<T>{i})) mark_refs(x);
        });
      }
    }
  }

  // Builds the compacted interners, in old index order per kind, and moves them into `ins`.
  Remap finish(Interners& ins)
    pre(&ins == ins_)
  {
    for (u32 k = 0; k < Nodes::size; ++k) {
      u32 next = 0;
      for (u32& i : remap_.nodes_[k])
        if (i != Remap::dead) i = next++;
    }
    for (auto& t : remap_.seqs_) {
      u32 next = 0;
      for (u32& i : t)
        if (i != Remap::dead) i = next++;
    }
    Ast ast;
    for (u32 k = 0; k < Nodes::size; ++k) {
      dispatch<Nodes>(k, [&]<class T>(std::type_identity<T>) {
        for (u32 i = 0; i < ins.ast.size(k); ++i) {
          if (remap_.nodes_[k][i] == Remap::dead) continue;
          NodeId old = make_node(k, i);
          T v = ins.ast.get<T>(old);
          for_each_ref(v, [&]<class H>(H& h) {
            if constexpr (std::is_same_v<H, ChildList>) {
              std::vector<Child> cs;
              for (Child c : ins.ast.children(h)) cs.push_back({remap_.map(c.id), c.at});
              h = ast.append_edges(cs);
            } else if constexpr (std::is_same_v<H, NodeId> || is_seq_id<H>) {
              h = remap_.map(h);
            }
          });
          NodeId n = ast.insert(v, ins.ast.width(old), ins.ast.fp(old));
          bool as_numbered = n == remap_.map(old);  // BOOTSTRAP: contracts: names a member (Interners::intern)
          contract_assert(as_numbered);
        }
      });
    }
    Seqs seqs;
    dispatch_each<SeqElements>([&]<class T>(std::type_identity<T>) {
      auto& live = remap_.seqs_[SeqElements::index_of<T>()];
      for (u32 i = 0; i < live.size(); ++i) {
        if (live[i] == Remap::dead) continue;
        std::vector<T> xs = ins.seqs.elements(SeqId<T>{i});
        for (T& x : xs) remap_.rewrite(x);
        SeqId<T> s = seqs.insert<T>(xs, ins.seqs.fp(SeqId<T>{i}));
        contract_assert(s.index == live[i]);
      }
    });
    ins.ast = std::move(ast);
    ins.seqs = std::move(seqs);
    rewrite_in_place<TypeKinds>(ins.types.table());
    rewrite_in_place<ValueKinds>(ins.values.table());
    return std::move(remap_);
  }

private:
  template <class List>
  void rewrite_in_place(KindTable<List>& table) {
    for (u32 n = 0; n < table.size(); ++n)
      dispatch<List>(table.kind(n), [&]<class T>(std::type_identity<T>) {
        T v = table.template get<T>(n);
        remap_.rewrite(v);
        table.replace(n, v);
      });
  }

  template <class List, class F>
  static void dispatch_each(F&& f) {
    [&]<u32... I>(std::integer_sequence<u32, I...>) {
      (f(std::type_identity<typename List::template at<I>>{}), ...);
    }(std::make_integer_sequence<u32, List::size>{});
  }

  template <class V>
  void mark_refs(const V& v) {
    for_each_ref(v, [this]<class H>(const H& h) {
      if constexpr (std::is_same_v<H, NodeId>) {
        mark_node(h);
      } else if constexpr (std::is_same_v<H, ChildList>) {
        for (const Child& c : ins_->ast.children(h)) mark_node(c.id);
      } else if constexpr (is_seq_id<H>) {
        mark_seq(h);
      }
    });
  }
  void mark_node(NodeId n) {
    if (n == NodeId::none) return;
    u32& m = remap_.nodes_[kind_of(n)].at(index_of(n));
    if (m != Remap::dead) return;
    m = 0;  // live; numbered in finish
    nodes_.push_back(n);
  }
  template <class T>
  void mark_seq(SeqId<T> s) {
    u32& m = remap_.seqs_[SeqElements::index_of<T>()].at(s.index);
    if (m != Remap::dead) return;
    m = 0;
    seqs_.push_back({SeqElements::index_of<T>(), s.index});
  }

  const Interners* ins_;
  Remap remap_;
  std::vector<NodeId> nodes_;
  std::vector<std::pair<u32, u32>> seqs_;
};

// Compacts `ins.ast` and `ins.seqs`; types and constants are roots, and the lists they hold are
// rewritten in place. `roots(c)` calls `c.mark(v)` for every live value held
// outside the interners; afterwards each holder applies the returned `Remap` to those values.
template <class Roots>
  requires std::invocable<Roots&, Compactor&>
Remap compact(Interners& ins, Roots&& roots) {
  Compactor c(ins);
  c.mark_interned();
  roots(c);
  return c.finish(ins);
}

}  // namespace ycxx::ast
