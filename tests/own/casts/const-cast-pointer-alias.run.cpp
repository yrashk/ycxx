// #1424, [expr.const.cast]: pointer casts keep the designated object and null value.
template<class T>
T* writable(const T* pointer) {
  return const_cast<T*>(pointer);
}

int value = 3;
int calls = 0;

const int* observe() {
  ++calls;
  return &value;
}

int main() {
  int* alias = writable(observe());
  if (alias != &value || calls != 1) return 1;
  *alias = 7;

  const void* read_only = alias;
  volatile void* volatile_view = alias;
  const volatile void* both = alias;
  if (const_cast<void*>(read_only) != alias ||
      const_cast<void*>(volatile_view) != alias ||
      const_cast<void*>(both) != alias) return 2;
  *static_cast<int*>(const_cast<void*>(both)) = 13;

  const int* null = nullptr;
  return value == 13 && const_cast<int*>(null) == nullptr ? 0 : 3;
}
