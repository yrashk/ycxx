using N = decltype(nullptr);

static_assert(N{} == nullptr);
static_assert(!(nullptr != N{}));
constexpr N constant_null = nullptr;
static_assert(constant_null == N{});
static_assert(!(nullptr != constant_null));

constexpr bool compare_volatile_local() {
  volatile N local = nullptr;
  N ordinary = nullptr;
  return local == ordinary && !(ordinary != local);
}
static_assert(compare_volatile_local());

namespace {
N left_object = nullptr;
const N right_object = nullptr;
int left_calls;
int right_calls;

N& left_null() {
  ++left_calls;
  return left_object;
}

const N& right_null() {
  ++right_calls;
  return right_object;
}
}

int main() {
  N local = nullptr;
  const N constant = nullptr;
  volatile N observed = nullptr;

  if (!(local == constant) || local != constant)
    return 1;
  if (!(constant == observed) || observed != constant)
    return 2;
  if (!(observed == N{}) || nullptr != observed)
    return 3;
  if (!(nullptr == local) || constant != N{})
    return 4;

  int lhs = 0;
  int rhs = 0;
  const bool equal = (++lhs, local) == (++rhs, observed);
  if (!equal || lhs != 1 || rhs != 1)
    return 5;
  const bool unequal = (++lhs, observed) != (++rhs, constant);
  if (unequal || lhs != 2 || rhs != 2)
    return 6;

  const bool calls_equal = left_null() == right_null();
  if (!calls_equal || left_calls != 1 || right_calls != 1)
    return 7;
  const bool calls_unequal = right_null() != left_null();
  if (calls_unequal || left_calls != 2 || right_calls != 2)
    return 8;

  if (!((local = nullptr) == local))
    return 9;
  if (local != (local = nullptr))
    return 10;
}
