// The handle vocabulary (ast.md §2): packing, the closed list, and the interner each handle
// enum names in its annotation (§12's probe of annotations on enumeration types).
import std;
import ycxx.ast.handles;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

static_assert(interner_of(^^NameId) == interner::names);
static_assert(interner_of(^^BodyId) == interner::tokens);
static_assert(interner_of(^^NodeId) == interner::ast);
static_assert(interner_of(^^QualType) == interner::types);
static_assert(names_an_interner(^^EntityId));
static_assert(!names_an_interner(^^DeclOrd));  // structural, not interned
static_assert(is_handle<NodeId> && is_handle<SeqId<NodeId>> && !is_handle<DeclId> && !is_handle<Loc>);
static_assert(handles::size == 8 && handles::index_of<NodeId>() == 6);
static_assert(sizeof(DeclId) == 8 && sizeof(Loc) == 8 && sizeof(NodeId) == 4 && sizeof(QualType) == 4);

int main() {
  NodeId n = make_node(17, 12345);
  check(kind_of(n) == 17 && index_of(n) == 12345, "kind_of(n) == 17 && index_of(n) == 12345");
  check(kind_of(make_node(0, max_node_index)) == 0 && index_of(make_node(0, max_node_index)) == max_node_index, "kind_of(make_node(0, max_node_index)) == 0 && index_of(make_node(0, max_node_index)) == max_node_index");

  QualType q = make_qual_type(1000, Cv::const_);
  check(type_node_of(q) == 1000 && cv_of(q) == Cv::const_, "type_node_of(q) == 1000 && cv_of(q) == Cv::const_");
  check(cv_of(with_cv(q, Cv::volatile_)) == Cv::const_volatile && type_node_of(with_cv(q, Cv::volatile_)) == 1000, "cv_of(with_cv(q, Cv::volatile_)) == Cv::const_volatile && type_node_of(with_cv(q, Cv::volatile_)) == 1000");

  DeclOrd o = make_decl_ord(DeclClass::synthesized, 5);
  check(class_of(o) == DeclClass::synthesized && number_of(o) == 5, "class_of(o) == DeclClass::synthesized && number_of(o) == 5");
  check((DeclId{ItemId{3}, o} == DeclId{ItemId{3}, o}) && !(DeclId{ItemId{3}, o} == DeclId{ItemId{4}, o}), "(DeclId{ItemId{3}, o} == DeclId{ItemId{3}, o}) && !(DeclId{ItemId{3}, o} == DeclId{ItemId{4}, o})");
  return ycxx::unit::result("ast.handles");
}
