// #1417 review [special]/6, Itanium C++ ABI §1.1 (POD for the purpose of layout): a user-provided
// copy assignment operator whose constraints are not satisfied is ineligible and does not keep
// the class from being POD, so a derived class does not reuse its tail padding.
template<class T> struct A {
  int i;
  char c;
  A& operator=(const A&) requires (sizeof(T) == 4) = default;
  A& operator=(const A&) { return *this; }
};
struct Derived : A<int> { char d; };
int main() { return sizeof(Derived) == 12 ? 0 : 1; }
