// ι (#298): range-based for over a class with no begin/end members: `begin(__range)` and
// `end(__range)` are looked up by argument-dependent lookup only, so the non-member functions in
// the class's namespace are found from a loop outside that namespace ([stmt.ranged]/1.3,
// [basic.lookup.argdep]). A class with a member named neither begin nor end, and a const
// overload chosen for a const range.
// XFAIL: ycxx non-member begin and end are found by argument-dependent lookup (#165)
// EXIT: 0
namespace bag {
struct Bag {
  int items[3];
  int count;
  Bag() : count(3) {
    items[0] = 2;
    items[1] = 3;
    items[2] = 5;
  }
};

int* begin(Bag& b) { return b.items; }
int* end(Bag& b) { return b.items + b.count; }

int const_begins = 0;
const int* begin(const Bag& b) {
  ++const_begins;
  return b.items;
}
const int* end(const Bag& b) { return b.items + b.count; }

// A class whose begin/end return an iterator declared in another namespace.
struct Pair {
  int first;
  int second;
  int size() const { return 2; }
};
}  // namespace bag

namespace pairs {
struct Walk {
  const bag::Pair* p;
  int i;
  int operator*() const { return i == 0 ? p->first : p->second; }
  Walk& operator++() {
    ++i;
    return *this;
  }
  bool operator!=(const Walk& o) const { return i != o.i; }
};
}  // namespace pairs

namespace bag {
pairs::Walk begin(const Pair& p) { return pairs::Walk{&p, 0}; }
pairs::Walk end(const Pair& p) { return pairs::Walk{&p, p.size()}; }
}  // namespace bag

int main() {
  bag::Bag b;
  int sum = 0;
  for (int x : b) sum += x;
  if (sum != 10) return 1;

  for (int& x : b) x *= 3;
  if (b.items[0] != 6 || b.items[2] != 15) return 2;

  const bag::Bag& cb = b;
  sum = 0;
  for (auto x : cb) sum += x;
  if (sum != 30) return 3;
  if (bag::const_begins != 1) return 4;

  bag::Pair p{7, 11};
  sum = 0;
  for (int x : p) sum = sum * 100 + x;
  if (sum != 711) return 5;
  return 0;
}
