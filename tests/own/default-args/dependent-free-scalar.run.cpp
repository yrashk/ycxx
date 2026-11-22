// EXIT: 0
template<class T>
T add(T x, T y = T(5)) { return x + y; }

template<class T>
int pick(int value = T::value) { return value; }

struct WithValue { static constexpr int value = 8; };
struct WithoutValue {};

int main() {
  if (add(1) != 6 || add('a') != 'a' + 5 || add<long>(3) != 8L) return 1;
  if (pick<WithoutValue>(4) != 4 || pick<WithValue>() != 8) return 2;
  return 0;
}
