// ζ s7 (#88), from the step's review: a static data member's in-class initializer is instantiated only when the member is used
// ([temp.inst]/3): Y<int>'s is invalid, and never needed.
// EXIT: 0
template<class T> struct Y { static constexpr int v = T::value; int x; };
int main() { Y<int> y; y.x = 0; return y.x; }
