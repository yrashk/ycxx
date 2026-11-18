// ζ c2 (#88): a class template specialization is implicitly instantiated only when a completely
// defined type is needed. Pointers to Bad<int>, whose member declaration would be ill-formed for
// int, and a function declaration with a Bad<long>& parameter never instantiate it; a template
// only declared so far can be named in pointers and declarations, and once defined, used
// completely ([temp.inst]/2, [temp.class], [basic.def.odr], [basic.types.general]). The
// pointers are never passed to an unqualified call: argument-dependent lookup looks into the
// pointee's class, which may instantiate it ([basic.lookup.argdep]).
// EXIT: 0
template<class T>
struct Bad {
  typename T::type member;
};

template<class T>
struct Later;

int touch(Bad<long>& r);

Later<int>* global = nullptr;

int read(Later<int>* p);

template<class T>
struct Later {
  T v;
};

int read(Later<int>* p) { return p->v; }

int main() {
  Bad<int>* p = nullptr;
  if (p != nullptr) return 1;
  Bad<int>** pp = &p;
  if (*pp != nullptr) return 5;
  Bad<char>* q = nullptr;
  if (q != nullptr) return 2;
  if (global != nullptr) return 3;
  Later<int> l{7};
  global = &l;
  if (read(global) != 7) return 4;
  return 0;
}
