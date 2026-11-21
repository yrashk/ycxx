// [dcl.stc], [expr.ref]
mutable int at_namespace;  //! error

struct ConstField { mutable const int field; };  //! error
struct ConstArray { mutable const int field[1]; };  //! error
struct ReferenceField { mutable int& field; };  //! error
struct StaticField { mutable static int field; };  //! error
struct Function { mutable int member(); };  //! error

struct Counter { mutable int calls; int fixed; };
void change(const Counter& c) {
    ++c.fixed;  //! error
}
