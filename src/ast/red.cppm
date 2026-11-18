// The red view (docs/design/ast.md §3.5): a cursor on the stack holding the interner, a node, its
// item-relative token offset and the parent cursor. Nodes hold no position, so a shared node has
// a different offset at each occurrence, and its parent is only ever the cursor chain of the
// current walk. A red view is never interned or stored.
export module ycxx.ast.red;

import std;
import ycxx.ast.handles;
import ycxx.ast.schema;
import ycxx.ast.nodes;
import ycxx.ast.interner;

export namespace ycxx::ast {

class Red {
public:
  // The root of a part sits at offset 0 of the part (§2, invariant 2); `offset` places the part in
  // its item (a body after the declaration part, §3.4).
  Red(const Ast& ast, NodeId root, u32 offset = 0)
    pre(ast.contains(root))
      : ast_(&ast), id_(root), offset_(offset), parent_(nullptr) {}

  NodeId id() const { return id_; }
  u32 kind() const { return kind_of(id_); }
  u32 offset() const { return offset_; }
  u32 width() const { return ast_->width(id_); }
  u32 end() const { return offset_ + width(); }
  const Red* parent() const { return parent_; }
  const Ast& ast() const { return *ast_; }

  template <class T>
    requires is_node_kind<T>
  bool is() const { return ast_->is<T>(id_); }

  template <class T>
    requires is_node_kind<T>
  T get() const { return ast_->get<T>(id_); }

  // Calls `f(const Red&)` for each child, in member order, then list order; absent children
  // (`NodeId::none`, in a `Child` or a list) are skipped. The child cursors live only during the
  // call.
  template <class F>
  void for_each_child(F&& f) const {
    dispatch<Nodes>(kind(), [&]<class T>(std::type_identity<T>) {
      T v = get<T>();
      template for (constexpr auto m : members_of<T>()) {
        using M = typename[:std::meta::type_of(m):];
        if constexpr (std::is_same_v<M, Child>) {
          if (v.[:m:].id != NodeId::none) f(child(v.[:m:]));
        } else if constexpr (std::is_same_v<M, ChildList>) {
          for (const Child& c : ast_->children(v.[:m:]))
            if (c.id != NodeId::none) f(child(c));
        }
      }
    });
  }

  // Descends to the innermost node whose extent [offset, end) holds `at`, and calls `f` with its
  // cursor, whose parent chain leads back here. Returns false, without calling `f`, when `at` is
  // outside this node.
  template <class F>
  bool find(u32 at, F&& f) const {
    if (at < offset_ || at >= end()) return false;
    bool found = false;
    for_each_child([&](const Red& c) {
      if (!found && c.find(at, f)) found = true;
    });
    if (!found) f(*this);
    return true;
  }

private:
  Red(const Ast* ast, NodeId id, u32 offset, const Red* parent)
      : ast_(ast), id_(id), offset_(offset), parent_(parent) {}

  Red child(Child c) const { return Red(ast_, c.id, offset_ + c.at, this); }

  const Ast* ast_;
  NodeId id_;
  u32 offset_;
  const Red* parent_;
};

}  // namespace ycxx::ast
