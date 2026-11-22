// Substituted member-pointer predicates select distinct pointer overloads (#209).
// EXIT: 0
template<bool, bool, bool> struct Tag {};

template<class T>
using Signature = Tag<__is_member_pointer(T), __is_member_object_pointer(T), __is_member_function_pointer(T)>;

template<class T> using Staged = Signature<T>;

struct Owner {
  int data;
};
using Data = int Owner::* const volatile;
using Method = int (Owner::*)(short) const & noexcept;
using Ordinary = int (*)(short);

int select(Tag<true, true, false>*) { return 1; }
int select(Tag<true, false, true>*) { return 2; }
int select(Tag<false, false, false>*) { return 3; }

template<class T> int classify() {
  Staged<T>* argument = nullptr;
  return select(argument);
}

int main() {
  if (classify<Data>() != 1)
    return 1;
  if (classify<Method>() != 2)
    return 2;
  if (classify<Ordinary>() != 3)
    return 3;
  if (classify<Method&>() != 3)
    return 4;
  return 0;
}
