// The reflection-derived schema (ast.md §2, §3.7, §9.6): sequencing read from annotations, the
// checks that keep the invariants (each probed to reject what it must), seeds and holds_nodes.
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

namespace {

// ---------------------------------------------------------------- sequencing ([intro.execution])

static_assert(is_expression(^^Binary) && is_expression(^^Call) && !is_expression(^^Return) && !is_expression(^^If));

static_assert(operands<Binary>.size() == 2 && relation<Binary>(0, 1) == Relation::unsequenced);
// [expr.assign]: the right operand is sequenced before the left.
static_assert(relation<Assign>(0, 1) == Relation::after && relation<Assign>(1, 0) == Relation::before);
// [expr.shift], [expr.log.and], [expr.comma]: left before right.
static_assert(relation<OrderedBinary>(0, 1) == Relation::before);
static_assert(relation<Subscript>(0, 1) == Relation::before);
// [expr.call]: the callee before every argument; arguments indeterminately sequenced.
static_assert(operands<Call>.size() == 2 && !operands<Call>[0].list && operands<Call>[1].list);
static_assert(relation<Call>(0, 1) == Relation::before);
static_assert(relation<Call>(1, 1) == Relation::indeterminately_sequenced);

// ---------------------------------------------------------------- the checks reject what they must

consteval bool node_rejected(std::meta::info kind) {
  try {
    check_node_kind(kind);
    return false;
  } catch (const std::meta::exception&) {
    return true;
  }
}
consteval bool record_rejected(std::meta::info type) {
  try {
    check_record(type);
    return false;
  } catch (const std::meta::exception&) {
    return true;
  }
}

struct WithPointer { const int* p; };
struct WithLoc { Loc where; };
struct WithFloat { double d; };
struct TooBig { Child a, b, c, d, e; };
struct Unannotated { ExprInfo info; Child operand; };
struct Disagreeing { ExprInfo info; [[=sequenced]] Child a; [[=unsequenced]] Child b; };
struct SequencedStructure { [[=sequenced]] Child a; };
struct AnnotatedNonOperand { ExprInfo info; [[=sequenced]] u32 n; };
enum class [[=interner::ast]] Unlisted : u32 {};
struct WithUnlistedHandle { Unlisted h; };
struct RecordWithEdge { Child c; };

static_assert(node_rejected(^^WithPointer));
static_assert(node_rejected(^^WithLoc));
static_assert(node_rejected(^^WithFloat));
static_assert(node_rejected(^^TooBig));  // §9.6: at most 32 bytes
static_assert(node_rejected(^^Unannotated));
static_assert(node_rejected(^^Disagreeing));
static_assert(node_rejected(^^SequencedStructure));
static_assert(node_rejected(^^AnnotatedNonOperand));
static_assert(node_rejected(^^WithUnlistedHandle));  // `handles` is closed
static_assert(record_rejected(^^RecordWithEdge));    // records hold NodeIds, not edges
static_assert(!node_rejected(^^Call) && !record_rejected(^^DeclSema));

// ---------------------------------------------------------------- holds_nodes and seeds

static_assert(holds_nodes_v<DeclSema> && holds_nodes_v<ItemSema> && holds_nodes_v<SeqId<ParamSema>>);
static_assert(!holds_nodes_v<BaseSpec> && !holds_nodes_v<MemberRec> && holds_nodes_v<ClassDef>);  // synthesized
static_assert(holds_nodes_v<BodySema> && holds_nodes_v<LocalRec>);

struct SameShapeA { u32 x; };
struct SameShapeB { u32 x; };
struct Renamed { u32 y; };
static_assert(schema_seed_v<SameShapeA> != schema_seed_v<SameShapeB>);  // the kind is identity
static_assert(schema_seed_v<Binary> != schema_seed_v<OrderedBinary>);
consteval Fp128 seed_of_shape_a_renamed() { return schema_seed(^^Renamed); }
static_assert(seed_of_shape_a_renamed() != schema_seed_v<SameShapeA>);

}  // namespace

int main() {
  // The operand table is usable at run time, as the IR generator reads it.
  check(operands<Assign>[0].seq.rank == 1 && operands<Assign>[1].seq.rank == 0, "operands<Assign>[0].seq.rank == 1 && operands<Assign>[1].seq.rank == 0");
  check(operands<Call>[1].seq.order == Order::indeterminately_sequenced, "operands<Call>[1].seq.order == Order::indeterminately_sequenced");
  return ycxx::unit::result("ast.schema");
}
