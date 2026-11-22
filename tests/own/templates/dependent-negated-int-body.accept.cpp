namespace N { template<class> inline constexpr bool flag = false; }
template<int> struct Tag {};

template<class T>
void f() { Tag<!::N::flag<T>> x; }

int main() { f<int>(); }
