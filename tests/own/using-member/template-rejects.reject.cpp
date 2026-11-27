// Member using-declarations in class templates: with a dependent base, a name the instantiated
// base does not declare, or declares privately, is rejected when the template is instantiated
// ([namespace.udecl]/1, /14, [temp.res.general]); with only nondependent bases, naming a class
// that is not a base is rejected ([namespace.udecl]/3), here also instantiated so that the
// diagnostic is required. (#1150)
struct HasCall { int operator()(int); };
struct NoCall { int f(); };
class Private { int operator()(int); };
template <class Fn> struct D : Fn {
  using Fn::operator();  //! error
};
template <class Fn> struct P : Fn {
  using Fn::operator();  //! error
};
struct Other { int g(); };
template <class T> struct N : HasCall {
  using Other::g;        //! error
};
D<HasCall> ok;
D<NoCall> bad;
P<Private> hidden;
N<int> nondependent;
int main() { return 0; }
