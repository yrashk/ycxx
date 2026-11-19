// ι (#291): each specialization of a function template is a distinct function, with its own
// static block variables ([temp.spec.general], [stmt.dcl]/3); calls of the same
// specialization share one variable. The same holds for member functions of class template
// specializations.
// EXIT: 0
template <class T>
int count() {
  static int n = 0;
  return ++n;
}

template <int N>
int scaled() {
  static int n = N * 10;
  return n += N;
}

template <class T>
struct Box {
  int next() {
    static int n = sizeof(T);
    return n++;
  }
};

template <class T>
T* slot() {
  static T value;
  return &value;
}

int main() {
  if (count<int>() != 1) return 1;
  if (count<int>() != 2) return 2;
  if (count<char>() != 1) return 3;
  if (count<long>() != 1) return 4;
  if (count<int>() != 3) return 5;
  if (count<char>() != 2) return 6;

  if (scaled<1>() != 11) return 7;
  if (scaled<2>() != 22) return 8;
  if (scaled<1>() != 12) return 9;
  if (scaled<2>() != 24) return 10;

  Box<char> bc;
  Box<int> bi;
  Box<int> bi2;
  if (bc.next() != 1) return 11;
  if (bi.next() != (int)sizeof(int)) return 12;
  if (bi2.next() != (int)sizeof(int) + 1) return 13;
  if (bc.next() != 2) return 14;

  *slot<int>() = 5;
  *slot<long>() = 7;
  if (*slot<int>() != 5 || *slot<long>() != 7) return 15;
  if (slot<int>() != slot<int>()) return 16;
  return 0;
}
