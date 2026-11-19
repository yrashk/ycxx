// ζ m9 (#88, #314), from the step's review: two template-heads declare only a member template of a
// class template ([temp.mem]/1).
template<class X> template<class Y> int g(Y) { return 1; } //! error
int main() { return 0; }
