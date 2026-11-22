// #209 [temp.arg.nontype], [temp.constr.constr], [temp.deduct.general].
template<int Code>
struct Token {};

template<class T> requires (sizeof(T) == 1)
consteval int token_code() { return 7; }

template<class T>
using TokenFor = Token<token_code<T>()>;

template<class T>
int resolve(T*, TokenFor<T>*);
long resolve(...);

using Pair = char[2];
static_assert(__is_same(decltype(resolve(static_cast<char*>(nullptr), static_cast<Token<7>*>(nullptr))), int));
static_assert(__is_same(decltype(resolve(static_cast<Pair*>(nullptr), static_cast<Token<7>*>(nullptr))), long));
