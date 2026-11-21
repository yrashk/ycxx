// Naming a specialization through a pointer alone leaves its base unchecked ([temp.inst]).
template<class Base> struct Derived : Base {};

Derived<int>* pass_pointer(Derived<int>* pointer) {
  return pointer;
}
