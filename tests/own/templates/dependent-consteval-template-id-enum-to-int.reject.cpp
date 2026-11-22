// #209 [dcl.enum], [temp.dep.constexpr], [temp.arg.nontype].
enum class Mode { single = 3 };

template<class T>
consteval Mode select_mode() { return Mode::single; }

template<int Selected>
struct Layout {};

template<class T>
using LayoutFor = Layout<select_mode<T>()>; //! error

using Rejected = LayoutFor<char>;
