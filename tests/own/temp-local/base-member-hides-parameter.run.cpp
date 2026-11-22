// #800: in a class template, and in a definition of its member outside it, the class's scope
// with its nondependent bases is searched before the template parameter scope, so a member of a
// nondependent base (or of the class, outside its definition) hides a template parameter of the
// same name ([temp.local]/7). A member template's own parameters, a dependent base's members and
// a non-template class's member templates are not affected. Each check agrees with Clang and GCC.
// EXIT: 0
struct Bs { static constexpr int I = 1; };
struct Bt { using T = int; };
struct Bf { static int I() { return 3; } };
struct B { static constexpr int v = 7; };
struct Mid : Bs {};

// The issue's example: a value member of the base hides a non-type parameter.
template<bool I> struct Value : Bs { static int get() { return I; } };
// A type member hides a type parameter, in a body and in a member's declaration.
template<class T> struct Type : Bt { static int get() { return sizeof(T); } T x; };
// The base's injected-class-name hides a type parameter.
template<class B> struct Injected : ::B { static int get() { return B::v; } };
// A member function of the base.
template<int I> struct Function : Bf { static int get() { return I(); } };
// A member of an indirect nondependent base.
template<bool I> struct Indirect : Mid { static int get() { return I; } };
// In the class's own declarations: a static member's initializer, a default member initializer,
// a return type.
template<bool I> struct Static : Bs { static constexpr int v = I; };
template<bool I> struct Default : Bs { int x = I; };
template<bool I> struct Signature : Bs { static decltype(I) g() { return 7; } };
// In a lambda in a member function.
template<bool I> struct Lambda : Bs { static int get() { return [](int x) { return x + I; }(10); } };
// Outside the class: a member of the class itself, and of its base, hides the parameter of the
// definition's head; a member template's own parameter is still found.
template<class T> struct Own { using U = int; static int f(); };
template<class U> int Own<U>::f() { return sizeof(U); }
template<bool I> struct Outside : Bs { static int get(); template<int J> static int g(); };
template<bool I> int Outside<I>::get() { return I; }
template<bool I> template<int J> int Outside<I>::g() { return I * 10 + J; }
template<class T> struct Param : Bt { static int f(T); };
template<class T> int Param<T>::f(T x) { return sizeof(x); }
// A base reached through a nondependent specialization, whose own base was dependent in its pattern.
template<class U> struct Through : U {};
template<class T> struct Via : Through<Bt> { static int get() { return sizeof(T); } T x; };

// Not hidden: a member template's own parameter, inside the class or a non-template class.
template<class T> struct MemberTemplate : Bs { template<int I> static int get() { return I; } };
struct Plain : Bs { template<int I> static int get() { return I; } };
// An explicit specialization has no template-head: its member template's parameter is found first.
template<class T> struct Explicit {};
template<> struct Explicit<int> { static constexpr int I = 1; template<int I> static int get() { return I; } };
// Not hidden: a dependent base is not searched ([temp.dep.general]).
template<int> struct DepI { static constexpr int I = 9; };
template<class T> struct DepT { static constexpr int I = 9; };
template<int I> struct Dependent : DepI<I> { static int get() { return I; } };
template<class T, int I> struct DependentType : DepT<T> { static int get() { return I; } };

int main() {
  if (Value<false>::get() != 1) return 1;
  if (Type<char>::get() != sizeof(int) || sizeof(Type<char>) != sizeof(int)) return 2;
  if (Injected<int>::get() != 7) return 3;
  if (Function<0>::get() != 3) return 4;
  if (Indirect<false>::get() != 1) return 5;
  if (Static<false>::v != 1) return 6;
  if (Default<false>{}.x != 1) return 7;
  if (Signature<false>::g() != 7) return 8;
  if (Lambda<false>::get() != 11) return 9;
  if (Own<char>::f() != sizeof(int)) return 10;
  if (Outside<false>::get() != 1) return 11;
  if (Outside<false>::g<3>() != 13) return 12;
  if (MemberTemplate<int>::get<5>() != 5) return 13;
  if (Plain::get<5>() != 5) return 14;
  if (Dependent<3>::get() != 3) return 15;
  if (DependentType<int, 3>::get() != 3) return 16;
  if (Param<char>::f(0) != sizeof(int)) return 17;
  if (Via<char>::get() != sizeof(int) || sizeof(Via<char>) != sizeof(int)) return 18;
  if (Explicit<int>::get<5>() != 5) return 19;
  return 0;
}
