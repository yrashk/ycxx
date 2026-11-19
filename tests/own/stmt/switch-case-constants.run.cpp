// #337 (ι): case values are converted constant expressions ([stmt.switch]/2): negative values,
// constexpr variables, enumerators, arithmetic, character literals, constexpr function calls.
// EXIT: 0
constexpr int Base = 40;
const int Other = 7;
enum Code { Low = -20, High = 20 };
enum class Mode { A = 3, B = 4 };
constexpr int twice(int x) { return x * 2; }

int pick(int v) {
  switch (v) {
    case -1: return 1;
    case -2147483647 - 1: return 2;
    case Base: return 3;
    case Base + 1: return 4;
    case Other: return 5;
    case Low: return 6;
    case High: return 7;
    case 1 + 2: return 8;
    case twice(50): return 9;
    case 'A': return 10;
    case static_cast<int>(Mode::B): return 11;
    case sizeof(int) == 4 ? 1000 : 1001: return 12;
    case (5 << 4) | 1: return 13;
  }
  return 0;
}

int main() {
  if (pick(-1) != 1) return 1;
  if (pick(-2147483647 - 1) != 2) return 2;
  if (pick(40) != 3) return 3;
  if (pick(41) != 4) return 4;
  if (pick(7) != 5) return 5;
  if (pick(-20) != 6) return 6;
  if (pick(20) != 7) return 7;
  if (pick(3) != 8) return 8;
  if (pick(100) != 9) return 9;
  if (pick(65) != 10) return 10;
  if (pick(4) != 11) return 11;
  if (pick(sizeof(int) == 4 ? 1000 : 1001) != 12) return 12;
  if (pick(81) != 13) return 13;
  if (pick(0) != 0) return 14;
  if (pick(-3) != 0) return 15;
  return 0;
}
