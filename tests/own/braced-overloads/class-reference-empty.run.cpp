// #503: an empty braced argument initializes a class temporary
// with a zero-initialized member ([dcl.init.list], [dcl.init.ref]).
// EXIT: 0
struct Record {
  int value;
};

int read(const Record& record) { return record.value; }

int main() {
  return read({}) != 0;
}
