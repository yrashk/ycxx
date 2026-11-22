// #798 companion [temp.func.order], [temp.deduct.partial], [temp.deduct.type].
template<int I, class... T>
int choose(T...[I]*, T...);
template<int I, class U, class... T>
long choose(U*, T...);

static_assert(__is_same(decltype(choose<1>(static_cast<long*>(nullptr), 0, 0L)), int));
