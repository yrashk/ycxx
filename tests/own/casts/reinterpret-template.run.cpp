// #199, [expr.reinterpret.cast], [temp.res]: casts in function templates, with dependent and
// non-dependent operands and targets, check the values each instantiation produces.
// The byte views assume a little-endian target, which every ycxx target is.
// EXIT: 0
template <class T> unsigned long length(const T* s) {
  return __builtin_strlen(reinterpret_cast<const char*>(s));
}

template <class T> const unsigned char* bytes(const T* p) {
  return reinterpret_cast<const unsigned char*>(p);
}

template <class T> T& view(unsigned& u) { return reinterpret_cast<T&>(u); }

// A non-dependent operand and target inside a template.
template <class T> T widened(signed char& c) { return T(reinterpret_cast<char&>(c)); }

template <class T> unsigned long bits(T* p) { return reinterpret_cast<unsigned long>(p); }
template <class T> T* restored(unsigned long b) { return reinterpret_cast<T*>(b); }

template <class T> T&& moved(T& x) { return reinterpret_cast<T&&>(x); }

struct Header { unsigned tag; unsigned size; };
template <class T> T* first_member(Header& h) { return reinterpret_cast<T*>(&h); }

int main() {
  if (length("abc") != 3) return 1;
  const unsigned char text[] = {'h', 'i', 0};
  if (length(text) != 2) return 2;

  unsigned word = 0x04030201u;
  const unsigned char* b = bytes(&word);
  if (b[0] != 1 || b[1] != 2 || b[3] != 4) return 3;
  if (static_cast<const void*>(b) != &word) return 4;

  int& signed_view = view<int>(word);
  signed_view = -1;
  if (word != 0xffffffffu) return 5;

  signed char c = 'x';
  if (widened<int>(c) != 'x') return 6;
  view<int>(word) = 5;
  if (word != 5u) return 7;

  int items[2] = {11, 22};
  unsigned long address = bits(items + 1);
  int* back = restored<int>(address);
  if (back != items + 1 || *back != 22) return 8;

  int value = 9;
  int&& r = moved(value);
  if (&r != &value) return 9;

  Header h{7, 8};
  unsigned* tag = first_member<unsigned>(h);
  if (tag != &h.tag || *tag != 7) return 10;
  *tag = 70;
  if (h.tag != 70) return 11;
  return 0;
}
