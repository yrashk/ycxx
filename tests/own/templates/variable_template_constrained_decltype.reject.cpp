template<class T>
concept SignedInt = __is_same(T, int);

template<SignedInt T>
constexpr int token = 19;

using Failed = decltype(+token<unsigned int>); //! error
