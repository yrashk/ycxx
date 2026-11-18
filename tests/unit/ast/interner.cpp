// Interning (ast.md §2, §9): hash-consing, Merkle fingerprints independent of interning order,
// width as identity, lists, names, and handles of placeholder interners.
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

namespace {

struct Tree {
  NodeId block, sum, sum2, call;
};

// `{ f(x + 1); return x + 1; }`, tokens: { f ( x + 1 ) ; return x + 1 ; }, offsets from `{`.
// `reverse` interns the leaves in the other order; fingerprints must not notice.
Tree build(Interners& in, bool reverse) {
  QualType int_t = in.intern_type(TBuiltin{Builtin::int_});
  EntityId f_e = in.entities.add<EntityId>(seed("f"));
  EntityId x_e = in.entities.add<EntityId>(seed("x"));
  ConstId one_c = in.intern_value(VInt{int_t, bits_of(std::int64_t{1})});
  ExprInfo rv{int_t, ValueCategory::prvalue, 0};
  NodeId one, x;
  if (reverse) {
    one = in.intern(Literal{rv, one_c}, 1);
    x = in.intern(DeclRef{{int_t, ValueCategory::lvalue, 0}, x_e}, 1);
  } else {
    x = in.intern(DeclRef{{int_t, ValueCategory::lvalue, 0}, x_e}, 1);
    one = in.intern(Literal{rv, one_c}, 1);
  }
  NodeId sum = in.intern(Binary{rv, BinaryOp::add, {x, 0}, {one, 2}}, 3);
  NodeId f = in.intern(DeclRef{{int_t, ValueCategory::lvalue, 0}, f_e}, 1);
  std::array args{Child{sum, 2}};
  NodeId call = in.intern(Call{rv, f_e, {f, 0}, {0, 1}}, 6, args);
  NodeId stmt = in.intern(ExprStmt{{call, 0}}, 7);
  NodeId sum2 = in.intern(Binary{rv, BinaryOp::add, {x, 0}, {one, 2}}, 3);
  NodeId ret = in.intern(Return{{sum2, 1}, in.intern_seq<Cleanup>({})}, 5);
  std::array stmts{Child{stmt, 1}, Child{ret, 8}};
  NodeId block = in.intern(Compound{{0, 2}, in.intern_seq<Cleanup>({})}, 14, stmts);
  return {block, sum, sum2, call};
}

}  // namespace

int main() {
  Interners a, b;
  Tree ta = build(a, false);
  Tree tb = build(b, true);

  // Equal subtrees are one node.
  check(ta.sum == ta.sum2, "ta.sum == ta.sum2");
  // Fingerprints are Merkle and independent of interning order; handles are not.
  check(a.ast.fp(ta.block) == b.ast.fp(tb.block), "a.ast.fp(ta.block) == b.ast.fp(tb.block)");
  check(a.ast.fp(ta.block) != a.ast.fp(ta.sum), "a.ast.fp(ta.block) != a.ast.fp(ta.sum)");

  // Re-interning the same tree adds nothing.
  u32 nodes = a.ast.node_count(), edges = a.ast.edge_count();
  Tree again = build(a, true);
  check(again.block == ta.block && a.ast.node_count() == nodes && a.ast.edge_count() == edges, "again.block == ta.block && a.ast.node_count() == nodes && a.ast.edge_count() == edges");

  // Reading back: payload, width, children.
  Call c = a.ast.get<Call>(ta.call);
  check(a.ast.width(ta.call) == 6 && a.ast.children(c.args).size() == 1 && a.ast.children(c.args)[0].id == ta.sum, "a.ast.width(ta.call) == 6 && a.ast.children(c.args).size() == 1 && a.ast.children(c.args)[0].id == ta.sum");
  check(a.ast.is<Call>(ta.call) && !a.ast.is<Binary>(ta.call), "a.ast.is<Call>(ta.call) && !a.ast.is<Binary>(ta.call)");

  // Width is identity: the same content over a different extent is another node.
  Binary s = a.ast.get<Binary>(ta.sum);
  NodeId wider = a.intern(s, 4);
  check(wider != ta.sum && a.ast.fp(wider) != a.ast.fp(ta.sum), "wider != ta.sum && a.ast.fp(wider) != a.ast.fp(ta.sum)");

  // Re-interning a stored node as a new one, with its lists read from the edge pool itself while
  // the pool grows (it never moves).
  Call stored = a.ast.get<Call>(ta.call);
  for (int i = 0; i < 64; ++i) a.intern(Literal{s.info, a.intern_value(VInt{a.intern_type(TBuiltin{Builtin::int_}), bits_of(std::int64_t{i})})}, 1);
  NodeId moved = a.intern_stored(stored, 7);
  check(moved != ta.call && a.ast.children(a.ast.get<Call>(moved).args).size() == 1 &&
            a.ast.children(a.ast.get<Call>(moved).args)[0].id == ta.sum,
        "a node interned from the pool's own edges keeps its children");

  // An edge offset is identity too: `x+1` and `x + 1` differ.
  NodeId tight = a.intern(Binary{s.info, s.op, s.lhs, {s.rhs.id, 1}}, 3);
  check(tight != ta.sum, "tight != ta.sum");

  // Lists: hash-consed, ordered, fingerprinted through their elements.
  EntityId e1 = a.entities.add<EntityId>(seed("e1"));
  EntityId e2 = a.entities.add<EntityId>(seed("e2"));
  SeqId<EntityId> l12 = a.intern_seq({e1, e2});
  check(a.intern_seq({e1, e2}) == l12 && !(a.intern_seq({e2, e1}) == l12), "a.intern_seq({e1, e2}) == l12 && !(a.intern_seq({e2, e1}) == l12)");
  check(a.seqs.size(l12) == 2 && a.seqs.at(l12, 1) == e2, "a.seqs.size(l12) == 2 && a.seqs.at(l12, 1) == e2");
  check(a.intern_seq<EntityId>({}) == a.intern_seq<EntityId>({}), "a.intern_seq<EntityId>({}) == a.intern_seq<EntityId>({})");
  // Lists of different element types never share an identity.
  check(a.fp(a.intern_seq<EntityId>({})) != a.fp(a.intern_seq<QualType>({})), "a.fp(a.intern_seq<EntityId>({})) != a.fp(a.intern_seq<QualType>({}))");

  // Records in lists, fingerprinted member by member.
  NameId n = a.names.intern("value");
  check(a.names.intern("value") == n && a.names.spelling(n) == "value" && a.names.intern("other") != n, "a.names.intern(\"value\") == n && a.names.spelling(n) == \"value\" && a.names.intern(\"other\") != n");
  SeqId<MemberRec> ms = a.intern_seq({MemberRec{{a.items.add<ItemId>(seed("item")), make_decl_ord(DeclClass::syntactic, 1)}, e1, Access::public_}});
  check(a.seqs.at(ms, 0).entity == e1 && a.seqs.at(ms, 0).access == Access::public_, "a.seqs.at(ms, 0).entity == e1 && a.seqs.at(ms, 0).access == Access::public_");
  SeqId<MemberRec> ms2 = a.intern_seq({MemberRec{{a.items.add<ItemId>(seed("item")), make_decl_ord(DeclClass::syntactic, 1)}, e1, Access::private_}});
  check(!(ms2 == ms) && a.fp(ms2) != a.fp(ms), "!(ms2 == ms) && a.fp(ms2) != a.fp(ms)");

  // A handle's fingerprint is its interner's, with cv in a QualType's.
  QualType t = a.intern_type(TBuiltin{Builtin::long_});
  check(a.fp(t) != a.fp(with_cv(t, Cv::const_)), "a.fp(t) != a.fp(with_cv(t, Cv::const_))");
  check(a.fp(NodeId::none) == a.fp(EntityId::none), "a.fp(NodeId::none) == a.fp(EntityId::none)");
  return ycxx::unit::result("ast.interner");
}
