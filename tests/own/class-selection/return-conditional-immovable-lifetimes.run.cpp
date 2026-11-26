// [stmt.return]/2,5, [expr.cond]/1,6,7.1, [class.temporary]/4,11
unsigned history;

void mark(unsigned event) noexcept {
  history = history * 10 + event;
}

struct Marker {
  unsigned end;

  Marker(unsigned begin, unsigned finish) noexcept : end(finish) { mark(begin); }
  ~Marker() noexcept { mark(end); }

  bool choose(bool left) const noexcept { return left; }
  int payload(int value) const noexcept { return value; }
};

struct Result {
  int payload;

  explicit Result(int value) noexcept : payload(value) { mark(5); }
  Result(const Result&) = delete;
  Result(Result&&) = delete;
  ~Result() noexcept { mark(9); }
};

Result choose_result(bool left) noexcept {
  Marker local{1, 8};
  return Marker{2, 7}.choose(left) ? Result{Marker{3, 6}.payload(41)}
                                  : Result{Marker{4, 6}.payload(73)};
}

bool check_result(bool left, int payload, unsigned before_destruction) noexcept {
  history = 0;
  {
    Result result = choose_result(left);
    if (result.payload != payload || history != before_destruction) return false;
  }
  return history == before_destruction * 10 + 9;
}

int main() {
  bool left = check_result(true, 41, 1235678);
  bool right = check_result(false, 73, 1245678);
  if (!left) return 1;
  if (!right) return 2;
  return 0;
}
