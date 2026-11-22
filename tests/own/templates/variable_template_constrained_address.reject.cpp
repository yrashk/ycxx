namespace probe {
template<class T>
requires (sizeof(T) > 1)
constexpr int token = 31;
}

const int* address = &probe::token<char>; //! error
