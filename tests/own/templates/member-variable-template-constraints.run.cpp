// #340: private static data member templates of a class template as libycxx's basic_string has
// them (`__sv_like`, `__char_ptr_iter`): named in the requires-clauses of member function
// templates, which choose overloads by them, and in `if constexpr` in their bodies. Each
// specialization of the class names its own ([temp.mem], [temp.inst]/7, [temp.constr.decl],
// [stmt.if]).
// EXIT: 0
template <class A, class B> inline constexpr bool same = false;
template <class A> inline constexpr bool same<A, A> = true;
template <class T> struct remove_ref { using type = T; };
template <class T> struct remove_ref<T&> { using type = T; };
template <class T> struct elem { using type = void; };
template <class T> struct elem<T*> { using type = T; };
template <class C> struct View { const C* p; };
template <class C> struct Str {
private:
  template <class T> static constexpr bool view_like = same<typename remove_ref<T>::type, View<C>>;
  template <class It> static constexpr bool char_ptr = same<typename elem<It>::type, C>;

public:
  int tag = 0;
  template <class T> requires view_like<T> int assign(const T&) { return tag = 1; }
  template <class T> requires (!view_like<T>) int assign(const T&) { return tag = 2; }
  template <class It> int append(It, It) {
    if constexpr (char_ptr<It>) return 10;
    else return 20;
  }
  static constexpr bool narrow = char_ptr<char*>;
};
int main() {
  Str<char> s;
  View<char> v{nullptr};
  if (s.assign(v) != 1 || s.assign(3) != 2) return 1;
  char c[2] = {};
  int w[2] = {};
  if (s.append(c, c + 1) != 10 || s.append(w, w + 1) != 20) return 2;
  Str<wchar_t> t;
  if (t.assign(View<wchar_t>{nullptr}) != 1 || t.assign(v) != 2 || t.append(c, c) != 20) return 3;
  return Str<char>::narrow && !Str<int>::narrow ? 0 : 4;
}
