// #800: a private member of a nondependent base hides a template parameter of the same name
// ([temp.local]/7); naming it is then an access error ([class.access]).
struct Bs { private: static constexpr int I = 1; };
template<int I> struct F : Bs {
  static int get() { return I; }  //! error
};
int main() { return F<9>::get(); }
