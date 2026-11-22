// Short-circuit the bad dependent atom before it becomes a checked constraint.
template<class T> concept Wrong = T{};
template<class T> concept SkipsWrong = true || Wrong<T>;
SkipsWrong auto value = 1;
static_assert(__is_same(decltype(value), int));
