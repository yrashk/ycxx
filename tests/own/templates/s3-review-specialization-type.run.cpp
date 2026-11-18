// ζ s3 (#88), from the step's review: a variable template specialization has the type its
// explicit or partial specialization declares, not the primary template's
// ([temp.expl.spec], [temp.spec.partial]).
// EXIT: 0
template<class T> int v = 0;
template<> long long v<int> = 0x100000007LL;
template<class T> long long v<T*> = 0x100000009LL;
long long get() { return v<int>; }
long long get_ptr() { return v<char*>; }
int main() {
  if (get() != 0x100000007LL) return 1;
  return get_ptr() == 0x100000009LL ? 0 : 2;
}
