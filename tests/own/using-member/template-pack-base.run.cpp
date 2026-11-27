// A pack expansion of using-declarators naming each dependent base's operator(), the "overloaded"
// idiom: `template<class... Fs> struct X : Fs... { using Fs::operator()...; };`, with class types
// and with closure types as bases ([namespace.udecl]/1, /3, [temp.variadic], [over.match.call]).
// (#1150)
// XFAIL: ycxx a pack of base classes and a pack expansion of using-declarators (#308)
struct I { int operator()(int) const { return 1; } };
struct C { int operator()(char) const { return 2; } };
struct P { int operator()(const char*) const { return 3; } };
template <class... Fs> struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <class... Fs> Overloaded<Fs...> overload(Fs... fs) { return {fs...}; }
int main() {
  Overloaded<I, C, P> o;
  auto l = overload([](int) { return 10; }, [](double) { return 20; });
  int bad = 0;
  if (o(1) != 1 || o('c') != 2 || o("s") != 3) bad |= 1;
  if (l(1) != 10 || l(1.5) != 20) bad |= 2;
  return bad;
}
