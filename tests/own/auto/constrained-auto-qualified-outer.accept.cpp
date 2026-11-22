// A qualified reference can still name the outer variable during deduction.
template<class T> concept C = true;
constexpr int x = 1;
namespace Inner {
  C auto x = ::x;
}
static_assert(__is_same(decltype(Inner::x), int));
