// δ s2 (#86): static data members with an out-of-class definition, `static constexpr` members,
// static member functions called as `S::f()` and through an object, static members used
// from non-static member functions, one static shared by all objects
// ([class.static], [class.static.data], [class.static.mfct]).
// EXIT: 0
struct Ticket {
  static int issued;
  static constexpr int limit = 3;
  static const int base = 100;
  int number;

  static bool available() { return issued < limit; }
  static int next() { return base + issued++; }
  bool take() {
    if (!available()) return false;
    number = next();
    return true;
  }
};

int Ticket::issued = 0;

static_assert(Ticket::limit == 3);

int main() {
  Ticket a, b, c, d;
  if (!a.take() || !b.take() || !c.take()) return 1;
  if (d.take()) return 2;
  if (a.number != 100 || c.number != 102) return 3;
  if (Ticket::issued != 3) return 4;
  if (a.issued != 3) return 5;  // the same object through any instance
  Ticket::issued = 1;
  if (!d.available() || !Ticket::available()) return 6;
  if (!d.take() || d.number != 101) return 7;
  int* p = &Ticket::issued;
  *p = 0;
  if (b.issued != 0) return 8;
  int arr[Ticket::limit] = {};
  if (arr[Ticket::limit - 1] != 0) return 9;
  return 0;
}
