// #209 [temp.arg.nontype], [temp.alias], [expr.const.const].
namespace Check {
template<signed char Index>
concept Fits = true;
}

struct Anchor {};

template<bool>
using Wrap = Anchor;

// Diagnostics can point at the base specifier or its instantiation.
template<int Index>
struct Record : Wrap<Check::Fits<Index>> {}; Record<1000> bad; //! error

Record<3> good;
