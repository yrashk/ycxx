// #209 [dcl.enum], [temp.dep.constexpr], [temp.arg.nontype].
enum class Mode : unsigned {
  single = 3,
  multiple = 9,
};

template<Mode Selected>
struct Layout {};

template<class T>
consteval Mode select_mode() {
  if (sizeof(T) == 1) return Mode::single;
  return Mode::multiple;
}

template<class T>
using LayoutFor = Layout<select_mode<T>()>;

template<class T>
struct Document : Layout<select_mode<T>()> {};

static_assert(__is_same(Layout<select_mode<char>()>, Layout<Mode::single>));
static_assert(__is_same(LayoutFor<char>, Layout<Mode::single>));
static_assert(__is_same(LayoutFor<char[3]>, Layout<Mode::multiple>));
static_assert(__is_base_of(Layout<Mode::single>, Document<char>));
static_assert(__is_base_of(Layout<Mode::multiple>, Document<char[3]>));
