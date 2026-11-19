// ζ a5 (#88): member typedefs and alias-declarations of a class and of a class template: types of
// the class's scope, named in later members, in member function bodies, as `C::T` outside, and as
// `typename X<T>::type` through a specialization ([class.mem], [dcl.typedef], [temp.res]).
// EXIT: 0
struct P {
  using I = int;
  typedef long L;
  I first;
  L second;
  I sum() const { return first + int(second); }
};
template<class T> struct Box {
  using value_type = T;
  typedef T* pointer;
  value_type v;
  pointer address() { return &v; }
  value_type get() const { return v; }
};
template<class T> struct Wrap { typedef typename Box<T>::value_type type; };
int main() {
  P p{1, 2L};
  P::I x = 3;
  P::L y = 4;
  Box<int> b{5};
  Box<int>::value_type c = *b.address();
  typename Wrap<long>::type w = 6;
  return p.sum() + x + y + c + b.get() + int(w) == 26 ? 0 : 1;
}
