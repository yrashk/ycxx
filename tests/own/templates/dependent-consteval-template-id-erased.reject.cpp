// #209 [temp.arg.nontype], [temp.constr.constr], [temp.alias].
template<class T> requires (sizeof(T) == 1)
consteval int index_for() { return 7; }

template<int>
using Erase = int;

template<class T>
using ErasedIndex = Erase<index_for<T>()>; //! error

static_assert(__is_same(ErasedIndex<char>, int));
using Rejected = ErasedIndex<char[2]>;
