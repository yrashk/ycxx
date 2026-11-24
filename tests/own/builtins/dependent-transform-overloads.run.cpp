// #1226: dependent type transforms of T and T* give the two overloads distinct names.
// XFAIL: gcc GCC 16 rejects __remove_cv in function template signatures.
// EXIT: 0
template<class T> int select(__remove_cv(T)* p) { return *p + 1; }
template<class T> int select(__remove_cv(T*)* p) { return **p + 2; }
int main() {
  int ordinary = 36;
  const volatile int qualified = 35;
  const volatile int* address = &qualified;
  return select<const volatile int>(&ordinary) == 37 &&
         select<const volatile int>(&address) == 37 ? 0 : 1;
}
