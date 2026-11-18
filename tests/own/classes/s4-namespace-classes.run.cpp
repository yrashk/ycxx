// δ s4 (#86): a class with member functions, a constructor, a static member and an enum in a
// namespace; the member functions and static member defined outside the namespace with
// qualified names; names in member function bodies looked up in the enclosing namespace
// ([namespace.memdef], [class.mfct], [class.static.data], [basic.lookup.unqual]).
// EXIT: 0
namespace inv {
  enum Kind { tool, part };
  int limit() { return 3; }

  class Bin {
    Kind kind_;
    int count_;

  public:
    static int bins;
    Bin(Kind k);
    bool add();
    int count() const { return count_; }
    Kind kind() const { return kind_; }
  };
}

int inv::Bin::bins = 0;

inv::Bin::Bin(Kind k) : kind_(k), count_(0) { ++bins; }

bool inv::Bin::add() {
  if (count_ >= limit()) return false;  // inv::limit, found from the member function's scope
  ++count_;
  return true;
}

int main() {
  inv::Bin a(inv::tool);
  inv::Bin b(inv::part);
  for (int i = 0; i < 5; i++) a.add();
  if (a.count() != 3) return 1;
  if (!b.add() || b.count() != 1) return 2;
  if (inv::Bin::bins != 2) return 3;
  if (a.kind() != inv::tool || b.kind() != inv::Kind::part) return 4;
  return 0;
}
