// ζ a5 (#88): an alias template's name is the only declaration of it in its scope, its arguments
// match its parameters, and a member alias is not declared twice ([temp.alias], [temp.arg],
// [class.mem]).
template<class T> using A = T*;
int A = 0; //! error
template<class T> using B = T;
B<int, int> b = 1; //! error
struct S {
  using T = int;
  typedef long T; //! error
};
int main() { return 0; }
