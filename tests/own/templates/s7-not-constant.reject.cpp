// ζ s7 (#88): a specialization's constexpr static data member is initialized by a constant
// expression; int has no member value ([class.static.data]/4, [temp.inst]/3).
template<class T> struct Y { static constexpr int v = T::value; }; //! error
int main() { return Y<int>::v; }
