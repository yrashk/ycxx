// ζ f1 (#88): deduction from pointer parameters: T* from int* and from const int* (T = const
// int), from an array (array-to-pointer conversion), from a pointer to a struct; const T* from
// int* deduces T = int and adds the const by a qualification conversion; T** from int**
// ([temp.deduct.call], [temp.deduct.type]).
// EXIT: 0
int id(int*) { return 1; }
int id(const int*) { return 2; }
int id(int**) { return 3; }

struct Point {
  int x;
  int y;
};

template<class T>
int ptr_kind(T* p) { return id(p); }

template<class T>
int cptr(const T* p) {
  T copy = *p;
  copy = copy + 1;
  return id(&copy) * 100 + copy;
}

template<class T>
int pp_kind(T** p) {
  T* inner = *p;
  return id(inner);
}

template<class T>
int sum_xy(T* p) { return p->x + p->y; }

template<class T>
T first(T* begin) { return *begin; }

int main() {
  int i = 4;
  const int ci = 9;
  int arr[3] = {7, 8, 9};
  int* p = &i;
  int* const cp = &i;
  const int* pc = &ci;
  if (ptr_kind(&i) != 1) return 1;
  if (ptr_kind(&ci) != 2) return 2;
  if (ptr_kind(arr) != 1) return 3;
  if (ptr_kind(cp) != 1) return 4;
  if (ptr_kind(pc) != 2) return 5;
  if (cptr(&i) != 105) return 6;
  if (cptr(&ci) != 110) return 7;
  if (cptr(arr) != 108) return 8;
  if (pp_kind(&p) != 1) return 9;
  Point pt{3, 4};
  if (sum_xy(&pt) != 7) return 10;
  if (first(arr) != 7) return 11;
  if (first(pc) != 9) return 12;
  return 0;
}
