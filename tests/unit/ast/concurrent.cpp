// Interning from several threads at once (#107; ast.md §2, §9): equal content interns to one
// handle whichever thread comes first, fingerprints equal a single-threaded run's, nothing is
// interned twice, readers see whole values while others append, lists longer than a storage
// block are whole, and compaction still works. Every pool grows across several blocks.
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

namespace {

constexpr int trees = 3000;
constexpr int threads = 8;

struct Result {
  NodeId block;
  Fp128 fp;
  QualType type;
  QualType canonical;
  NameId name;
  SeqId<QualType> params;
  NodeId again;  // the call, re-interned from the edge pool
};

// Tree `i`: `{ g_i(x_i + k); return x_i + k; }` over a type built from sugar, pointers, arrays and
// a function type, so types intern their canonical forms recursively. Trees share most of their
// subtrees (k and the types repeat), so threads race on the same fingerprints.
Result build(Interners& in, int i) {
  const int k = i % 7;
  QualType int_t = in.intern_type(TBuiltin{Builtin::int_});
  QualType t = int_t;
  for (int d = 0; d <= i % 4; ++d) t = in.intern_type(TPointer{t});
  EntityId alias = in.entities.add<EntityId>(seed(std::format("T{}", i % 5)));
  QualType sugar = in.intern_type(STypedef{t, alias}, Cv::const_);
  ConstId bound = in.intern_value(VInt{in.intern_type(TBuiltin{Builtin::unsigned_long}), bits_of(std::int64_t{2 + i % 3})});
  QualType arr = in.intern_type(TArray{sugar, bound});
  SeqId<QualType> params = in.intern_seq<QualType>({arr, int_t});
  QualType fn = in.intern_type(TFunction{int_t, params, ConstId::none, 0, Cv::none, RefQual::none, LangLinkage::cxx});
  // Every 97th tree also has a name, a list and a block longer than a storage block.
  const bool long_one = i % 97 == 0;
  NameId name = in.names.intern(long_one ? std::string(3000 + i, 'n') : std::format("name{}", i));
  if (long_one) {
    std::vector<QualType> many(2500 + i % 5, int_t);
    many.back() = arr;
    (void)in.intern_seq<QualType>(std::span<const QualType>(many));
  }

  EntityId g = in.entities.add<EntityId>(seed(std::format("g{}", i)));
  EntityId x = in.entities.add<EntityId>(seed(std::format("x{}", i % 11)));
  ConstId kc = in.intern_value(VInt{int_t, bits_of(std::int64_t{i})});
  (void)k;
  ExprInfo rv{int_t, ValueCategory::prvalue, 0};
  NodeId lit = in.intern(Literal{rv, kc}, 1);
  NodeId ref = in.intern(DeclRef{{int_t, ValueCategory::lvalue, 0}, x}, 1);
  NodeId sum = in.intern(Binary{rv, BinaryOp::add, {ref, 0}, {lit, 2}}, 3);
  NodeId callee = in.intern(DeclRef{{fn, ValueCategory::lvalue, 0}, g}, 1);
  std::array args{Child{sum, 2}};
  NodeId call = in.intern(Call{rv, g, {callee, 0}, {0, 1}}, 6, args);
  NodeId stmt = in.intern(ExprStmt{{call, 0}}, 7);
  NodeId ret = in.intern(Return{{sum, 1}, in.intern_seq<Cleanup>({})}, 5);
  std::vector<Child> stmts{Child{stmt, 1}, Child{ret, 8}};
  if (long_one) stmts.resize(2600, Child{stmt, 1});
  NodeId block = in.intern(Compound{{0, static_cast<u32>(stmts.size())}, in.intern_seq<Cleanup>({})}, 14, stmts);
  NodeId again = in.intern_stored(in.ast.get<Call>(call), 6);
  return {block, in.ast.fp(block), sugar, in.canonical(sugar), name, params, again};
}

}  // namespace

int main() {
  Interners reference;
  std::vector<Result> want;
  for (int i = 0; i < trees; ++i) want.push_back(build(reference, i));

  for (int round = 0; round < 3; ++round) {
    Interners in;
    std::vector<std::vector<Result>> got(threads, std::vector<Result>(trees));
    std::atomic<int> torn{0};
    {
      std::vector<std::jthread> workers;
      for (int w = 0; w < threads; ++w) {
        workers.emplace_back([&, w] {
          std::vector<int> order(trees);
          std::iota(order.begin(), order.end(), 0);
          std::ranges::shuffle(order, std::mt19937(static_cast<unsigned>(round * threads + w)));
          for (int i : order) {
            got[w][i] = build(in, i);
            // Read back what this thread holds while the others append.
            const Result& r = got[w][i];
            const std::string spelling = i % 97 == 0 ? std::string(3000 + i, 'n') : std::format("name{}", i);
            const u32 stmts = i % 97 == 0 ? 2600 : 2;
            if (in.ast.fp(r.block) != r.fp || in.names.spelling(r.name) != spelling || in.seqs.size(r.params) != 2 ||
                in.ast.children(in.ast.get<Compound>(r.block).stmts).size() != stmts) {
              ++torn;
            }
          }
        });
      }
    }
    int disagree = 0, wrong_fp = 0, wrong_canon = 0;
    for (int i = 0; i < trees; ++i) {
      for (int w = 1; w < threads; ++w) {
        const Result& a = got[0][i];
        const Result& b = got[w][i];
        disagree += a.block != b.block || a.type != b.type || a.canonical != b.canonical || a.name != b.name || a.params != b.params ||
                    a.again != b.again;
      }
      wrong_fp += got[0][i].fp != want[i].fp || in.types.fp(got[0][i].type) != reference.types.fp(want[i].type) ||
                  in.types.fp(got[0][i].canonical) != reference.types.fp(want[i].canonical);
      wrong_canon += in.canonical(got[0][i].type) != got[0][i].canonical || in.ast.fp(got[0][i].again) != reference.ast.fp(want[i].again);
    }
    check(disagree == 0, std::format("round {}: every thread got the same handles ({} differ)", round, disagree));
    check(wrong_fp == 0, std::format("round {}: fingerprints equal the single-threaded run's ({} differ)", round, wrong_fp));
    check(wrong_canon == 0 && torn == 0, std::format("round {}: canonical forms and reads consistent", round));
    check(in.ast.node_count() == reference.ast.node_count() && in.types.size() == reference.types.size() &&
              in.values.size() == reference.values.size() && in.names.size() == reference.names.size() &&
              in.seqs.count<QualType>() == reference.seqs.count<QualType>(),
          std::format("round {}: nothing interned twice", round));

    // Compaction at quiescence still works on what the threads built.
    std::vector<NodeId> roots;
    for (int i = 0; i < trees; i += 2) roots.push_back(got[0][i].block);
    Remap remap = compact(in, [&](Compactor& c) {
      for (NodeId n : roots) c.mark(n);
    });
    bool kept = true;
    for (int i = 0; i < trees; i += 2) kept = kept && in.ast.fp(remap.map(got[0][i].block)) == want[i].fp;
    check(kept, std::format("round {}: compaction keeps the marked trees", round));
  }
  return ycxx::unit::result("ast.concurrent");
}
