// #1567: after the class's constructor, `A<int, int>` is still a template-id of the injected
// template-name ([temp.local]/1), checked against its one parameter ([temp.arg.general]).
template <class T> struct A {
  A() = default;
  void f(const A<int, int>&);  //! error
};
int main() { return 0; }
