// ζ c6 (#88): an atom's parameter mapping holds only the parameters it names
// ([temp.constr.atomic]/1, [temp.constr.normal]/1.4): Ctrue's atom `true` names none, so
// `typename T::type` is never formed and the constraint is satisfied for int.
// EXIT: 0
template<class T> concept Ctrue = true;
template<class T> requires Ctrue<typename T::type> int f(T) { return 1; }
int f(long) { return 2; }
int main() { return f(1) == 1 ? 0 : 1; }
