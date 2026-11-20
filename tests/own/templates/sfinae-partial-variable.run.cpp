// #263: a dependent type in a variable partial specialization must substitute
// successfully and match the supplied type ([temp.spec.partial.match], [temp.deduct.general]).
// EXIT: 0
template<class T, class U> constexpr int pick = 1;
template<class T> constexpr int pick<T, typename T::result> = 2;

struct Valid { using result = int; };
struct Missing {};

int main() {
  if (pick<Valid, int> != 2) return 1;
  if (pick<Valid, long> != 1) return 2;
  if (pick<Missing, int> != 1) return 3;
  return 0;
}
