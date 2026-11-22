// A direct declaration stops this path through the using-directives.
template<class T> concept C = true;
namespace Inner {}
namespace Mid {
  using namespace Inner;
  constexpr int x = 2;
}
namespace Route {
  using namespace Mid;
}
namespace Inner {
  C auto x = Route::x;
}
static_assert(__is_same(decltype(Inner::x), int));
