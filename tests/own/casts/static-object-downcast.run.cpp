// [expr.static.cast], [conv.ptr], [class.access.base] (#199).
struct Prefix { int tag; };
struct Base { int value; };
struct Middle : Prefix, Base { int mid; };
struct Lead { long lead; };
struct Derived : Lead, Middle { int last; };
struct OuterLead { long marker; };
struct Outer : OuterLead, Derived { Base member; };

int calls;
Base* get_pointer(Base* p) { ++calls; return p; }
Base& get_lvalue(Base& r) { ++calls; return r; }
Derived* from_pointer(Base* const p) { return static_cast<Derived*>(get_pointer(p)); }
Derived& from_reference(Base& r) { return static_cast<Derived&>(get_lvalue(r)); }

int category(Derived&) { return 1; }
int category(Derived&&) { return 2; }
int category(const volatile Derived&) { return 3; }

class PrivateDerived : Base {
public:
  static bool check(PrivateDerived& d) {
    ::Base* b = &d;
    return static_cast<PrivateDerived*>(b) == &d;
  }
};

int main() {
  Outer outer{};
  Derived* d = &outer;
  Middle* m = d;
  Base* b = m;
  if (static_cast<void*>(d) == static_cast<void*>(&outer)) return 1;
  if (static_cast<void*>(d) == static_cast<void*>(m)) return 2;
  if (static_cast<void*>(m) == static_cast<void*>(b)) return 3;
  if (from_pointer(b) != d || calls != 1) return 4;
  if (from_pointer(nullptr) != nullptr || calls != 2) return 5;
  Derived& r = from_reference(*b);
  if (&r != d || calls != 3) return 6;
  r.last = 23;
  r.value = 19;
  if (outer.last != 23 || b->value != 19) return 7;
  if (static_cast<const volatile Derived*>(b) != d) return 8;
  if (category(static_cast<Derived&>(*b)) != 1 || category(static_cast<const volatile Derived&>(*b)) != 3) return 9;
  if (static_cast<Derived*>(static_cast<Middle*>(b)) != d) return 10;
  PrivateDerived private_derived{};
  if (!PrivateDerived::check(private_derived)) return 11;
  return 0;
}
