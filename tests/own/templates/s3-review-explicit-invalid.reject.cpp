// ζ s3 (#88), from the step's review: a member of an explicitly specialized class is defined
// without `template<>`, and `template<>` before a class-head needs template arguments
// ([temp.expl.spec]).
template<class T> struct S { int m(); };
template<> struct S<int> { int m(); };
template<> int S<int>::m() { return 3; } //! error
template<class T> struct X { };
template<> struct X; //! error
int main() { return 0; }
