template<class, class> constexpr bool same = false;
template<class T> constexpr bool same<T, T> = true;

struct Outer {};
Outer items;
Outer callback;
auto inspect(int items[4], long callback(short))
    noexcept(same<decltype(items), int*> && same<decltype(callback), long(*)(short)>)
    -> decltype(items);

static_assert(same<decltype(inspect(nullptr, nullptr)), int*>);
static_assert(same<decltype(items), Outer>);
static_assert(same<decltype(callback), Outer>);
