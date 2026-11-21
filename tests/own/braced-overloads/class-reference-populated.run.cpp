// #503: the temporary's initialized members are available during the call
// and through the full-expression containing it ([dcl.init.list], [class.temporary]).
// EXIT: 0
struct Record {
  int first;
  int second;
};

const int* find_second(const Record& record, int expected) {
  if (record.first != expected) return nullptr;
  return &record.second;
}

int check(int first, int second) {
  const int* member = nullptr;
  return (member = find_second({first, second}, first),
          member == nullptr || *member != second);
}

int main() {
  return check(-3, 8) + check(7, -2);
}
