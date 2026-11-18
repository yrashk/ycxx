// ζ c2 (#88), from the step's review: a class template that needs its own specialization of
// a longer argument is instantiated without end, and reaches the limit ([temp.inst]/16). A small
// limit for every compiler.
// FLAGS: -ftemplate-depth=16
template<class T> struct L { L<T*> next; }; //! error
int main() { L<int> l; return 0; }
