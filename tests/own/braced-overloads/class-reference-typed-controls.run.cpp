// #503: named objects and explicitly typed temporaries bind to the
// ordinary class const reference parameter. A single braced lvalue of the
// same type can bind to a mutable reference ([dcl.init.list], [dcl.init.ref]).
// EXIT: 0
struct Record {
  int first;
  int second;
};

int read(const Record& record) { return 10 * record.first + record.second; }
void edit(Record& record) { record.second += 3; }

int main() {
  Record named{-3, 8};
  if (read(named) != -22) return 1;
  if (read(Record{}) != 0) return 2;
  if (read(Record{-3, 8}) != -22) return 3;
  edit({named});
  return named.second != 11;
}
