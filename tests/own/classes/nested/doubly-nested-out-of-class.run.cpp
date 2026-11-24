// #306 [class.nest]/3: out-of-class definition of a class nested two levels deep.
struct A { struct B { struct C; }; };
struct A::B::C { int v; int get() const { return v; } };
int main() { A::B::C c{12}; return c.get() == 12 ? 0 : 1; }
