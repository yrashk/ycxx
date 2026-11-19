// ι (#313): pointers to functions stored in struct members, initialized from overload sets, called
// through `.` and `->`, and a null pointer checked before the call ([over.over],
// [dcl.init.aggr], [expr.ref], [expr.call], [conv.bool]).
// EXIT: 0
int add(int a, int b) { return a + b; }
int add(int a) { return a; }
int mul(int a, int b) { return a * b; }

struct Op {
  int (*fn)(int, int);
  int bias;
};

int run(const Op* op, int a, int b) {
  if (!op->fn) return -1;
  return op->fn(a, b) + op->bias;
}

int main() {
  Op ops[3] = {{add, 1}, {&mul, 0}, {nullptr, 5}};
  if (ops[0].fn(2, 3) != 5) return 1;
  if ((*ops[1].fn)(2, 3) != 6) return 2;
  if (run(&ops[0], 2, 3) != 6) return 3;
  if (run(&ops[1], 4, 5) != 20) return 4;
  if (run(&ops[2], 4, 5) != -1) return 5;
  ops[2].fn = add;
  if (run(&ops[2], 4, 5) != 14) return 6;
  Op o;
  o.fn = nullptr;
  o.bias = 0;
  if (o.fn) return 7;
  o.fn = &add;
  if (!o.fn) return 8;
  Op* p = &o;
  if (p->fn(1, 1) != 2) return 9;
  if ((*p->fn)(2, 2) != 4) return 10;
  return 0;
}
