// ζ s7 (#88), from the step's review: a static data member initialized in its class template cannot
// be given another initializer by an explicit specialization ([class.static.data]/4,
// [temp.expl.spec]).
template<class T> struct A { static const int v = sizeof(T); };
template<> const int A<char>::v = 30; //! error
int main() { return 0; }
