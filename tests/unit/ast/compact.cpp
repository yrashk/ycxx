// Compaction (ast.md §9.5): what the holders mark survives with its fingerprint, everything else
// goes, handles are renumbered and rewritten by the one reflection walk, and the rebuilt tables
// find the survivors again.
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

int main() {
  Interners in;
  QualType int_t = in.intern_type(TBuiltin{Builtin::int_});
  ExprInfo rv{int_t, ValueCategory::prvalue, 0};
  EntityId f_e = in.entities.add<EntityId>(seed("f"));
  auto lit = [&](const char* v) { return Literal{rv, in.intern_value(VInt{int_t, bits_of(std::int64_t{std::atoi(v)})})}; };

  // Garbage first, so the survivors' handles move.
  NodeId garbage = in.intern(lit("7"), 1);
  in.intern(Binary{rv, BinaryOp::mul, {garbage, 0}, {garbage, 2}}, 3);
  in.intern_seq({ParamSema{EntityId::none, int_t, in.names.intern("dead"), garbage, 0}});

  BodySema body;
  ItemSema item;
  {
    Builder b(in);
    NodeId one = b.add(lit("1"), 1);
    NodeId two = b.add(lit("2"), 1);
    NodeId f = b.add(DeclRef{rv, f_e}, 1);
    NodeId call = b.add(Call{rv, f_e, {f, 0}, b.list({{one, 2}, {two, 4}})}, 6);
    NodeId local = b.add(LocalVar{0, {call, 2}, EntityId::none}, 9);
    body = b.commit(BodySema{b.add(Compound{b.list({{local, 1}}), in.intern_seq<Cleanup>({})}, 11),
                             b.seq({LocalRec{in.names.intern("v"), int_t, EntityId::none, local, 0}})});
    DeclSema d{f_e, int_t, DeclKind::function, Linkage::external, Storage::none, Access::none, 0,
               b.seq<NodeId>({}), NodeId::none, NodeId::none,
               b.seq({ParamSema{EntityId::none, int_t, in.names.intern("p"), two, 0}})};
    item = b.commit(ItemSema{call, b.seq({d})});
  }
  Fp128 body_fp = in.ast.fp(body.tree), call_fp = in.ast.fp(item.tree);
  Fp128 locals_fp = in.fp(body.locals), decls_fp = in.fp(item.decls);
  u32 before = in.ast.node_count();

  Remap m = compact(in, [&](Compactor& c) {
    c.mark(body);
    c.mark(item);
  });
  check(!m.live(garbage), "!m.live(garbage)");
  m.rewrite(body);
  item = m(item);

  // Survivors keep their fingerprints and content; garbage is gone.
  check(in.ast.node_count() == before - 2, "in.ast.node_count() == before - 2");  // the 7 and the 7 * 7
  check(in.seqs.count<ParamSema>() == 1, "in.seqs.count<ParamSema>() == 1");
  check(in.ast.fp(body.tree) == body_fp && in.ast.fp(item.tree) == call_fp, "in.ast.fp(body.tree) == body_fp && in.ast.fp(item.tree) == call_fp");
  check(in.fp(body.locals) == locals_fp && in.fp(item.decls) == decls_fp, "in.fp(body.locals) == locals_fp && in.fp(item.decls) == decls_fp");
  LocalRec v = in.seqs.at(body.locals, 0);
  check(in.ast.is<LocalVar>(v.decl) && in.ast.get<LocalVar>(v.decl).init.id == item.tree, "in.ast.is<LocalVar>(v.decl) && in.ast.get<LocalVar>(v.decl).init.id == item.tree");
  Call c = in.ast.get<Call>(item.tree);
  check(in.ast.children(c.args).size() == 2 && in.ast.children(c.args)[1].at == 4, "in.ast.children(c.args).size() == 2 && in.ast.children(c.args)[1].at == 4");
  check(in.seqs.at(in.seqs.at(item.decls, 0).params, 0).default_arg == in.ast.children(c.args)[1].id, "in.seqs.at(in.seqs.at(item.decls, 0).params, 0).default_arg == in.ast.children(c.args)[1].id");

  // The rebuilt tables find the survivors: re-interning gives the compacted handle.
  check(in.intern(in.ast.get<Call>(item.tree), 6, in.ast.edges()) == item.tree, "in.intern(in.ast.get<Call>(item.tree), 6, in.ast.edges()) == item.tree");
  check(in.ast.node_count() == before - 2, "in.ast.node_count() == before - 2");
  return ycxx::unit::result("ast.compact");
}
