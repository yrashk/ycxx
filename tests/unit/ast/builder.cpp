// Frame builders (ast.md §9.3): trees are built as drafts in the frame arena, and committing a
// value interns exactly what it reaches, bottom-up. Abandoned drafts leave nothing behind; lists
// of records that hold nodes are interned with their tree, the rest eagerly.
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

int main() {
  Interners in;
  QualType int_t = in.intern_type(TBuiltin{Builtin::int_});
  ExprInfo rv{int_t, ValueCategory::prvalue, 0};
  ConstId one_c = in.intern_value(VInt{int_t, bits_of(std::int64_t{1})});
  ConstId two_c = in.intern_value(VInt{int_t, bits_of(std::int64_t{2})});
  EntityId f_e = in.entities.add<EntityId>(seed("f"));

  std::array<std::byte, 4096> buffer;
  std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size());

  // A speculative parse: `1 + 2` built, then abandoned for `f(1)`.
  ItemSema item;
  {
    Builder b(in, &arena);
    NodeId one = b.add(Literal{rv, one_c}, 1);
    NodeId two = b.add(Literal{rv, two_c}, 1);
    NodeId abandoned = b.add(Binary{rv, BinaryOp::add, {one, 0}, {two, 2}}, 3);
    check(b.get<Binary>(abandoned).rhs.id == two && b.width(abandoned) == 3, "b.get<Binary>(abandoned).rhs.id == two && b.width(abandoned) == 3");

    NodeId f = b.add(DeclRef{rv, f_e}, 1);
    NodeId call = b.add(Call{rv, f_e, {f, 0}, b.list({{one, 2}})}, 4);
    NodeId attr = b.add(Attribute{in.names.intern(""), in.names.intern("nodiscard"), b.list({})}, 1);

    // Lists of records holding nodes are drafts; lists of plain handles are interned now.
    SeqId<ParamSema> params = b.seq({ParamSema{EntityId::none, int_t, in.names.intern("p"), one, 0}});
    SeqId<EntityId> eager = b.seq({f_e});
    check(in.seqs.contains(eager) && in.seqs.at(eager, 0) == f_e, "in.seqs.contains(eager) && in.seqs.at(eager, 0) == f_e");
    check(in.seqs.count<ParamSema>() == 0, "in.seqs.count<ParamSema>() == 0");

    DeclSema d{f_e, int_t, DeclKind::function, Linkage::external, Storage::none, Access::none, 0,
               b.seq({attr}), NodeId::none, NodeId::none, params, b.seq(std::span<const TemplateParamSema>{})};
    check(in.ast.node_count() == 0, "in.ast.node_count() == 0");  // nothing interned before commit
    item = b.commit(ItemSema{call, b.seq({d}), b.seq(std::span<const ExpansionRec>{})});
  }

  // Exactly what the item reaches was interned: literal 1, f, the call, the attribute.
  check(in.ast.node_count() == 4, "in.ast.node_count() == 4");
  check(in.ast.size(Nodes::index_of<Binary>()) == 0, "in.ast.size(Nodes::index_of<Binary>()) == 0");
  check(in.ast.is<Call>(item.tree) && in.ast.width(item.tree) == 4, "in.ast.is<Call>(item.tree) && in.ast.width(item.tree) == 4");
  Call c = in.ast.get<Call>(item.tree);
  check(in.ast.children(c.args).size() == 1 && in.ast.is<Literal>(in.ast.children(c.args)[0].id), "in.ast.children(c.args).size() == 1 && in.ast.is<Literal>(in.ast.children(c.args)[0].id)");
  DeclSema d = in.seqs.at(item.decls, 0);
  check(in.seqs.size(d.attributes) == 1 && in.ast.is<Attribute>(in.seqs.at(d.attributes, 0)), "in.seqs.size(d.attributes) == 1 && in.ast.is<Attribute>(in.seqs.at(d.attributes, 0))");
  check(in.seqs.at(d.params, 0).default_arg == in.ast.children(c.args)[0].id, "in.seqs.at(d.params, 0).default_arg == in.ast.children(c.args)[0].id");

  // Committing the same tree from another frame gives the same handles: content is identity.
  {
    Builder b(in, &arena);
    NodeId one = b.add(Literal{rv, one_c}, 1);
    NodeId f = b.add(DeclRef{rv, f_e}, 1);
    NodeId call = b.add(Call{rv, f_e, {f, 0}, b.list({{one, 2}})}, 4);
    check(b.commit(call) == item.tree, "b.commit(call) == item.tree");
    check(in.ast.node_count() == 4, "in.ast.node_count() == 4");

    // Adopting an interned node, and committing a second root from the same builder.
    NodeId kept = b.adopt(item.tree);
    NodeId stmt = b.add(ExprStmt{{kept, 0}}, 5);
    NodeId s = b.commit(stmt);
    check(in.ast.get<ExprStmt>(s).expr.id == item.tree && in.ast.node_count() == 5, "in.ast.get<ExprStmt>(s).expr.id == item.tree && in.ast.node_count() == 5");
    check(b.commit(call) == item.tree, "b.commit(call) == item.tree");  // a draft is interned at most once
  }

  // Abandoning a whole frame interns nothing.
  {
    Builder b(in, &arena);
    b.add(Literal{rv, two_c}, 1);
  }
  check(in.ast.node_count() == 5, "in.ast.node_count() == 5");
  return ycxx::unit::result("ast.builder");
}
