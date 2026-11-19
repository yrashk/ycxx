// ζ a5 (#88), from the step's review: a member type is not named after its class, nor after a
// template parameter ([class.mem.general], [temp.local]/6).
struct S { using S = int; }; //! error
template<class T> struct R { using T = int; }; //! error
int main() { return 0; }
