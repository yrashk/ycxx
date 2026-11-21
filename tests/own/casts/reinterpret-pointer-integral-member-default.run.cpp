// #199, [expr.reinterpret.cast], [dcl.fct.default]: the comma inside the cast's
// template arguments belongs to the first default. The second default remains usable.
// EXIT: 0
template<class T, class U>
struct Pair {
  T first;
  U second;
};

Pair<int, long> item = {23, 41};

unsigned long item_bits() {
  return reinterpret_cast<unsigned long>(&item);
}

struct Reader {
  int read(Pair<int, long>* p = reinterpret_cast<Pair<int, long>*>(item_bits()),
           int extra = 7) {
    return p->first + extra;
  }
};

int main() {
  Reader reader;
  Pair<int, long> other = {14, 52};
  if (reader.read() != 30) return 1;
  if (reader.read(&other) != 21) return 2;
  return 0;
}
