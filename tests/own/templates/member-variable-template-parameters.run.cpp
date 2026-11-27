// #340: static data member templates with non-type parameters, default template arguments
// (`v<>`), parameter packs (`sizeof...`), and one member template's initializer naming another
// ([temp.mem], [temp.param], [temp.arg], [temp.variadic]).
// EXIT: 0
template <int K> struct S {
  template <int N> static constexpr int scaled = N * K;
  template <class T = int, int N = 2> static constexpr int dflt = int(sizeof(T)) * N;
  template <class... Ts> static constexpr int count = int(sizeof...(Ts)) + K;
  template <int N> static constexpr int chained = scaled<N> + count<char, char>;
};
struct N {
  template <class T = char> static constexpr int size = int(sizeof(T));
  template <class... Ts> static constexpr int many = int(sizeof...(Ts));
  template <int... Is> static constexpr int total = (0 + ... + Is);
};
static_assert(S<3>::scaled<4> == 12);
static_assert(S<3>::dflt<> == 8);
static_assert(S<3>::dflt<char> == 2);
static_assert(S<3>::dflt<short, 5> == 10);
static_assert(S<3>::count<> == 3);
static_assert(S<3>::count<int, long, char> == 6);
static_assert(S<5>::chained<2> == 17);
static_assert(N::size<> == 1);
static_assert(N::many<> == 0 && N::many<int, int> == 2);
static_assert(N::total<1, 2, 3> == 6);
int main() { return S<2>::scaled<3> + N::size<> == 7 ? 0 : 1; }
