// Class and enum categories select distinct pointer overloads (#209).
// EXIT: 0
template<bool, bool, bool> struct Tag {};

template<class T>
using Signature = Tag<__is_enum(T), __is_union(T), __is_class(T)>;

enum class State;
union Storage;
struct Entry;

int select(Signature<int>*) { return 1; }
int select(Signature<const State>*) { return 2; }
int select(Signature<Storage>*) { return 3; }
int select(Signature<Entry>*) { return 4; }

int main() {
  Tag<false, false, false>* other = nullptr;
  Tag<true, false, false>* enumeration = nullptr;
  Tag<false, true, false>* union_type = nullptr;
  Tag<false, false, true>* class_type = nullptr;

  if (select(other) != 1)
    return 1;
  if (select(enumeration) != 2)
    return 2;
  if (select(union_type) != 3)
    return 3;
  if (select(class_type) != 4)
    return 4;
  return 0;
}
