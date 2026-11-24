// #317: equivalent arguments name one parameter object across templates
// ([temp.param], [temp.arg.nontype]). Its address differs from the source's.
// EXIT: 0
struct Key {
  long value;
  bool active;
};

constexpr Key stored{9};

template<Key Value>
const Key* address() {
  return &Value;
}

template<Key Value>
const Key* other_address() {
  return &Value;
}

int main() {
  const Key* named = address<stored>();
  const Key* explicit_zero = other_address<Key{9, false}>();
  const Key* implicit_zero = address<Key{9}>();
  const Key* different = address<Key{9, true}>();

  if (named == &stored || named != explicit_zero || named != implicit_zero) return 1;
  if (named == different) return 2;
  if (named->value != 9 || named->active) return 3;
  return different->value == 9 && different->active ? 0 : 4;
}
