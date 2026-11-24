// #1226: unqualified lookup in a template definition skips a dependent direct base
// ([temp.res.general], [class.member.lookup]). Reduction from #1207.
template<class> using Pick = int;
template<class T> struct Base {
  template<class> using Pick = long long;
};
template<class T> struct D : Base<T> {
  static int result() { return sizeof(Pick<int>); }
};
int main() { return D<int>::result() == sizeof(int) ? 0 : 1; }
