namespace A { struct Base {}; int operator+(const Base&, int); }
namespace B { struct Derived : A::Base {}; }

int direct(const A::Base& b) { return b + 1; }
int derived(const B::Derived& d) { return d + 1; }
int qualified(const B::Derived& d) { return A::operator+(d, 1); }

namespace Friends {
struct Value { friend int operator-(Value, int); };
}
int hidden(Friends::Value v) { return v - 1; }

namespace Types { struct Value {}; }
int operator*(Types::Value, int);
int lexical(Types::Value v) { return v * 1; }
