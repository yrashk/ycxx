// δ s2 (#86): a fixed-capacity stack: member functions defined in the class, implicit member
// access, calls with `.` and `->`, a member function calling another one
// ([class.mfct], [class.mfct.non.static], [expr.ref]).
// EXIT: 0
struct Stack {
  int items[8];
  int top;

  void clear() { top = 0; }
  bool empty() { return top == 0; }
  bool full() { return top == 8; }
  bool push(int v) {
    if (full()) return false;
    items[top++] = v;
    return true;
  }
  int pop() { return items[--top]; }
  int peek() { return items[top - 1]; }
};

int drain(Stack* s) {
  int total = 0;
  while (!s->empty()) total += s->pop();
  return total;
}

int main() {
  Stack s;
  s.clear();
  if (!s.empty()) return 1;
  for (int i = 1; i <= 10; i++) s.push(i);
  if (!s.full()) return 2;
  if (s.peek() != 8) return 3;
  if (s.pop() != 8) return 4;
  if (s.top != 7) return 5;
  Stack* p = &s;
  p->push(100);
  if (p->peek() != 100) return 6;
  if (drain(&s) != 128) return 7;
  if (!s.empty()) return 8;
  return 0;
}
