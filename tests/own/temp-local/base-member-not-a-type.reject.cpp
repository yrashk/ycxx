// #800: a value member of a nondependent base hides a type parameter of the same name
// ([temp.local]/7), so the name is not a type there.
struct Bs { static constexpr int I = 1; };
template<class I> struct F : Bs {
  I x;  //! error
};
int main() { return 0; }
