// The red view (ast.md §3.5): item-relative offsets of each occurrence of a shared node, the
// parent chain of the current walk, and descent to the node at an offset.
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

int main() {
  Interners in;
  QualType int_t = in.intern_type(TBuiltin{Builtin::int_});
  ExprInfo rv{int_t, ValueCategory::prvalue, 0};
  ExprInfo lv{int_t, ValueCategory::lvalue, 0};
  EntityId f_e = in.entities.add<EntityId>(seed("f"));
  EntityId x_e = in.entities.add<EntityId>(seed("x"));

  // { f(x + 1); return x + 1; }   tokens: { f ( x + 1 ) ; return x + 1 ; }
  //                                offsets: 0 1 2 3 4 5 6 7  8     9 10 11 12 13
  Builder b(in);
  NodeId x = b.add(DeclRef{lv, x_e}, 1);
  NodeId one = b.add(Literal{rv, in.intern_value(VInt{int_t, bits_of(std::int64_t{1})})}, 1);
  NodeId sum = b.add(Binary{rv, BinaryOp::add, {x, 0}, {one, 2}}, 3);
  NodeId f = b.add(DeclRef{lv, f_e}, 1);
  NodeId call = b.add(Call{rv, f_e, {f, 0}, b.list({{sum, 2}})}, 6);
  NodeId stmt = b.add(ExprStmt{{call, 0}}, 7);
  NodeId sum2 = b.add(Binary{rv, BinaryOp::add, {x, 0}, {one, 2}}, 3);
  NodeId ret = b.add(Return{{sum2, 1}, in.intern_seq<Cleanup>({})}, 5);
  NodeId block = b.add(Compound{b.list({{stmt, 1}, {ret, 8}}), in.intern_seq<Cleanup>({})}, 14);
  NodeId root = b.commit(block);

  // Both `x + 1`s are one node, at two offsets.
  std::vector<std::pair<NodeId, u32>> sums;
  std::vector<u32> literal_offsets;
  auto walk = [&](this auto& self, const Red& r) -> void {
    if (r.is<Binary>()) sums.push_back({r.id(), r.offset()});
    if (r.is<Literal>()) literal_offsets.push_back(r.offset());
    r.for_each_child(self);
  };
  walk(Red(in.ast, root));
  check(sums.size() == 2 && sums[0].first == sums[1].first, "sums.size() == 2 && sums[0].first == sums[1].first");
  check(sums[0].second == 3 && sums[1].second == 9, "sums[0].second == 3 && sums[1].second == 9");
  check((literal_offsets == std::vector<u32>{5, 11}), "(literal_offsets == std::vector<u32>{5, 11})");

  // A body placed after a declaration part shifts every offset (§3.4).
  literal_offsets.clear();
  walk(Red(in.ast, root, 100));
  check((literal_offsets == std::vector<u32>{105, 111}), "(literal_offsets == std::vector<u32>{105, 111})");

  // Descent to an offset: token 11 is the second `1`, under return, under the block.
  bool found = Red(in.ast, root).find(11, [&](const Red& r) {
    check(r.is<Literal>() && r.offset() == 11 && r.width() == 1, "r.is<Literal>() && r.offset() == 11 && r.width() == 1");
    check(r.parent() && r.parent()->is<Binary>() && r.parent()->offset() == 9, "r.parent() && r.parent()->is<Binary>() && r.parent()->offset() == 9");
    check(r.parent()->parent()->is<Return>() && r.parent()->parent()->parent()->is<Compound>(), "r.parent()->parent()->is<Return>() && r.parent()->parent()->parent()->is<Compound>()");
    check(r.parent()->parent()->parent()->parent() == nullptr, "r.parent()->parent()->parent()->parent() == nullptr");
  });
  check(found, "found");
  // Token 7 (`;`) belongs to the statement, not to the call.
  check(Red(in.ast, root).find(7, [&](const Red& r) { check(r.is<ExprStmt>() && r.offset() == 1, "r.is<ExprStmt>() && r.offset() == 1"); }), "find(7)");
  check(!Red(in.ast, root).find(14, [](const Red&) {}), "!Red(in.ast, root).find(14, [](const Red&) {})");

  // An absent child in a list is skipped by the walk and by descent.
  Builder b2(in);
  NodeId lit = b2.add(Literal{rv, in.intern_value(VInt{int_t, bits_of(std::int64_t{1})})}, 1);
  NodeId gap = b2.commit(b2.add(Compound{b2.list({{NodeId::none, 1}, {lit, 3}}), in.intern_seq<Cleanup>({})}, 5));
  u32 children = 0;
  Red(in.ast, gap).for_each_child([&](const Red&) { ++children; });
  check(children == 1, "a none in a list is not a child");
  check(Red(in.ast, gap).find(1, [&](const Red& r) { check(r.is<Compound>(), "token 1 is the block's own"); }),
        "find over a list with a none");
  return ycxx::unit::result("ast.red");
}
