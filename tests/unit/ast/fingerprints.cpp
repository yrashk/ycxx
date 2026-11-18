// A handle's fingerprint is its interner's Merkle fingerprint, never its number (#68, ast.md §2,
// query-engine.md §A1.2): two interners that intern the same types in different orders give them
// different numbers and equal fingerprints, values holding handles fingerprint by content, and the
// local hash of in-memory tables follows the number.
import std;
import ycxx.ast;
import ycxx.unit;

using namespace ycxx::ast;
using ycxx::unit::check;

namespace {

// A value an engine might store: handles inside an aggregate and a sequence.
struct Held {
  QualType type;
  EntityId entity;
  SeqId<QualType> list;
  std::vector<QualType> more;
};

struct Built {
  QualType int_t, ptr, fn;
  EntityId e;
  SeqId<QualType> params;
};

// The same content, interned in one order or the other.
Built build(Interners& in, bool reversed) {
  Built b;
  if (reversed) {
    in.intern_type(TBuiltin{Builtin::char_});  // shifts the numbers of everything after it
    b.e = in.entities.add<EntityId>(seed("f"));
  }
  b.int_t = in.intern_type(TBuiltin{Builtin::int_});
  b.ptr = in.intern_type(TPointer{b.int_t});
  b.params = in.intern_seq(std::span<const QualType>(std::array{b.ptr, b.int_t}));
  b.fn = in.intern_type(TFunction{b.int_t, b.params, ConstId::none, 0, Cv::none, RefQual::none, LangLinkage::cxx});
  if (!reversed) b.e = in.entities.add<EntityId>(seed("f"));
  return b;
}

}  // namespace

int main() {
  Interners a, b;
  Built x = build(a, false), y = build(b, true);
  check(x.fn != y.fn && x.int_t != y.int_t, "different interning orders give different numbers");

  Held hx{x.fn, x.e, x.params, {x.ptr, x.int_t}}, hy{y.fn, y.e, y.params, {y.ptr, y.int_t}};
  ycxx::Fp128 fx, fy, local_x, local_y;
  {
    BindInterners bind(a);
    fx = ycxx::fingerprint(hx);
    local_x = ycxx::local_hash(hx);
    check(ycxx::fingerprint(x.fn) != ycxx::fingerprint(x.ptr), "different types, different fingerprints");
  }
  {
    BindInterners bind(b);
    fy = ycxx::fingerprint(hy);
    local_y = ycxx::local_hash(hy);
    // Bindings nest and restore.
    {
      BindInterners inner(a);
      check(ycxx::fingerprint(hx) == fx, "a nested binding");
    }
    check(ycxx::fingerprint(hy) == fy, "the outer binding is restored");
  }
  check(fx == fy, "equal content, equal fingerprints, whatever the numbers");
  check(local_x != local_y, "the local hash follows the numbers");

  // Another thread has its own binding.
  ycxx::Fp128 on_thread;
  std::thread([&] {
    BindInterners bind(b);
    on_thread = ycxx::fingerprint(hy);
  }).join();
  check(on_thread == fx, "a binding per thread");
  return ycxx::unit::result("unit.ast_fingerprints");
}
