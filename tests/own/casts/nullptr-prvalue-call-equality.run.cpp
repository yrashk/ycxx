using N = decltype(nullptr);

namespace {
int left_calls;
int right_calls;

N left_null() {
  ++left_calls;
  return nullptr;
}

N right_null() {
  ++right_calls;
  return N{};
}
}

int main() {
  const bool equal = left_null() == right_null();
  if (!equal || left_calls != 1 || right_calls != 1)
    return 1;

  const bool unequal = right_null() != left_null();
  if (unequal || left_calls != 2 || right_calls != 2)
    return 2;

  int object = 0;
  int* pointer = &object;
  const bool pointer_equal = pointer == left_null();
  if (pointer_equal || left_calls != 3 || right_calls != 2)
    return 3;

  int* initialized = right_null();
  if (initialized != nullptr || left_calls != 3 || right_calls != 3)
    return 4;
}
