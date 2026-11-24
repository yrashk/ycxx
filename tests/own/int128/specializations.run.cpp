// #1172, as reported: explicit specializations for both 128-bit types are distinct, which
// libycxx's __sign_pair needs (#776).
// FLAGS: -Wno-pedantic
template <class T> struct P;
template <> struct P<__int128> { int v = 1; };
template <> struct P<unsigned __int128> { int v = 2; };
int main() { return P<unsigned __int128>{}.v - 2; }
