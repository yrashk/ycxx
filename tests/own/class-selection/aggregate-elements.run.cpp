// #740 [expr.cond], [expr.comma], [dcl.init.aggr], [class.temporary].
// EXIT: 0
int events;

struct Marker {
  int digit;
  explicit Marker(int n) noexcept : digit(n) {}
  ~Marker() noexcept { events = events * 10 + digit; }
};

bool choose(Marker&&, bool first) noexcept {
  events = events * 10 + 1;
  return first;
}

void mark(Marker&&, int digit) noexcept { events = events * 10 + digit; }

struct Result {
  int value;
  Result(Marker&&, int n, int digit) noexcept : value(n) { events = events * 10 + digit; }
  Result(const Result&) = delete;
  Result(Result&&) = delete;
  ~Result() noexcept { events = events * 10 + 9; }
};

struct Box { Result selected; int observed; };
int observe() noexcept { return events; }

int check_field(bool first) {
  events = 0;
  {
    Box box{(mark(Marker(6), 7), choose(Marker(8), first) ? Result(Marker(3), 19, 2)
                                                        : Result(Marker(5), 31, 4)),
            observe()};
    if (box.selected.value != (first ? 19 : 31) || box.observed != (first ? 712 : 714)) return 1;
    if (events != (first ? 712386 : 714586)) return 2;
  }
  return events != (first ? 7123869 : 7145869);
}

int check_array(bool first) {
  events = 0;
  {
    Result values[2]{choose(Marker(8), first) ? Result(Marker(3), 19, 2) : Result(Marker(5), 31, 4),
                     first ? Result(Marker(7), 43, 6) : Result(Marker(2), 47, 7)};
    if (values[0].value != (first ? 19 : 31) || values[1].value != (first ? 43 : 47)) return 1;
    if (events != (first ? 126738 : 147258)) return 2;
  }
  return events != (first ? 12673899 : 14725899);
}

int main() {
  if (check_field(true)) return 1;
  if (check_field(false)) return 2;
  if (check_array(true)) return 3;
  return check_array(false) ? 4 : 0;
}
