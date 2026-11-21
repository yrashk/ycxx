// Four type categories select four distinct pointer overloads (#209).
// EXIT: 0
template<bool, bool, bool> struct Tag {};

template<class T>
using Signature = Tag<__is_array(T), __is_pointer(T), __is_function(T)>;

struct Record;
using MemberPointer = int Record::*;
using Array = const int[2][3];
using Function = int(short);
using FunctionPointer = int (*const)(short);

int select(Signature<MemberPointer>*) { return 1; }
int select(Signature<Array>*) { return 2; }
int select(Signature<FunctionPointer>*) { return 3; }
int select(Signature<Function>*) { return 4; }

int main() {
  Tag<false, false, false>* other = nullptr;
  Tag<true, false, false>* array = nullptr;
  Tag<false, true, false>* pointer = nullptr;
  Tag<false, false, true>* function = nullptr;

  if (select(other) != 1)
    return 1;
  if (select(array) != 2)
    return 2;
  if (select(pointer) != 3)
    return 3;
  if (select(function) != 4)
    return 4;
  return 0;
}
