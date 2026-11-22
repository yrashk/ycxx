// [stmt.return], [class.copy.elision]: the local has a different class type
// from the result, so initialization moves its base subobject before cleanup.
// EXIT: 0
int moves;
int moved_from_destructions;
int result_destructions;

struct Result {
  int value;

  Result(int n) : value(n) {}
  Result(const Result&) = delete;
  Result(Result&& other) : value(other.value) {
    ++moves;
    other.value = 0;
  }
  ~Result() {
    if (value == 0) ++moved_from_destructions;
    else ++result_destructions;
  }
};

struct Local : Result {
  Local(int value) : Result(value) {}
};

Result make_result() {
  Local local(47);
  return local;
}

int main() {
  {
    Result result = make_result();
    if (result.value != 47) return 1;
    if (moves != 1 || moved_from_destructions != 1) return 2;
    if (result_destructions != 0) return 3;
  }
  return moved_from_destructions != 1 || result_destructions != 1;
}
