// #285, spec-only. A hidden local and its lifetime-extended temporaries are each destroyed once ([dcl.struct.bind], [class.temporary]).
int next_calls;
int constructed;
int destroyed;
int last_sum;
int comma_calls;

struct Trace {
  int first;
  int second;

  Trace(int a, int b) : first(a), second(b) { ++constructed; }
  Trace(const Trace&) = delete;
  ~Trace() {
    ++destroyed;
    last_sum = first + second;
  }
};

struct Part { int value; };
struct Whole {
  Part part;
  ~Whole() {
    ++destroyed;
    last_sum = part.value;
  }
};

int next_value(int value) {
  ++next_calls;
  return value;
}

int main() {
  {
    auto [a, b] = Trace(next_value(2), 3);
    if (next_calls != 1 || constructed != 1 || destroyed != 0 || a != 2 || b != 3) return 1;
    b = 5;
  }
  if (destroyed != 1 || last_sum != 7) return 2;

  {
    const auto& [a, b] = Trace(next_value(7), 11);
    if (next_calls != 2 || constructed != 2 || destroyed != 1 || a != 7 || b != 11) return 3;
  }
  if (destroyed != 2 || last_sum != 18) return 4;

  {
    auto&& [a, b](Trace(next_value(13), 17));
    if (next_calls != 3 || constructed != 3 || destroyed != 2 || a != 13 || b != 17) return 5;
    a = 19;
  }
  if (destroyed != 3 || last_sum != 36) return 6;

  {
    const auto& [value] = (++comma_calls, Whole{{23}}.part);
    if (comma_calls != 1 || destroyed != 3 || value != 23) return 7;
  }
  if (destroyed != 4 || last_sum != 23) return 8;

  {
    const Part& part = (++comma_calls, Whole{{29}}.part);
    if (comma_calls != 2 || destroyed != 4 || part.value != 29) return 9;
  }
  if (destroyed != 5 || last_sum != 29) return 10;

  Part left{31}, right{37};
  bool choose_left = true;
  auto& [selected] = choose_left ? left : right;
  if (&selected != &left.value) return 11;
}
