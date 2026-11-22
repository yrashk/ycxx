// #740 [expr.cond], [expr.comma], [class.copy.elision], [class.temporary].
// EXIT: 0
int events;
const int* selected_value;
int observed_value;

struct Marker {
  int event;
  explicit Marker(int n) noexcept : event(n) {}
  ~Marker() noexcept {
    events = 10 * events + event;
    if (selected_value) observed_value = *selected_value;
  }
};

bool choose(Marker&&, bool first) noexcept {
  events = 10 * events + 1;
  return first;
}

struct Result {
  int value;
  Result(Marker&&, int n, int event) noexcept : value(n) {
    selected_value = &value;
    events = 10 * events + event;
  }
  Result(const Result&) = delete;
  Result(Result&&) = delete;
  ~Result() noexcept {
    events = 10 * events + 9;
    selected_value = nullptr;
  }
};

int check_conditional(bool first) {
  events = 0;
  observed_value = 0;
  {
    Result result = choose(Marker(8), first) ? Result(Marker(3), 19, 2)
                                             : Result(Marker(5), 31, 4);
    if (result.value != (first ? 19 : 31) || observed_value != result.value) return 1;
    if (events != (first ? 1238 : 1458)) return 2;
  }
  return events != (first ? 12389 : 14589);
}

int check_comma() {
  events = 0;
  observed_value = 0;
  {
    Result result = (choose(Marker(8), true), Result(Marker(3), 19, 2));
    if (result.value != 19 || observed_value != 19 || events != 1238) return 1;
  }
  return events != 12389;
}

int main() {
  if (check_conditional(true)) return 1;
  if (check_conditional(false)) return 2;
  return check_comma() ? 3 : 0;
}
