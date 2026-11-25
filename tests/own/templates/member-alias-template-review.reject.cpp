// #340 item 2, from the review: a member alias template's access is checked where it is named,
// also as a template template argument ([class.access]/4); it hides a base's member of its name,
// and an earlier member of the class hides a base's alias template ([class.member.lookup]).
class P {
  template <class U> using apply = U;
};
template <template <class> class G> struct Use {
  using type = G<int>;
};
P::apply<int> a = 0;  //! error
Use<P::apply>::type b = 0;  //! error
struct B {
  using apply = int;
  static constexpr int value = 3;
  template <class U> using ptr = U*;
};
struct D : B {
  template <class U> using apply = U*;
  template <class U> using value = U;
  apply y = 0;  //! error
};
D::apply c = 0;  //! error
int d = D::value;  //! error
struct E : B {
  static int ptr(int);
  ptr<int> p;  //! error
};
int main() { return 0; }
