// δ s1 (#86): a ring buffer: an array member, indexing through `.` and `->`, a zero-initialized
// global of struct type ([class.mem], [expr.sub], [expr.ref], [basic.start.static]).
// EXIT: 0
struct Ring {
  int data[4];
  int head;
  int count;
};

Ring g;

bool put(Ring* r, int v) {
  if (r->count == 4) return false;
  r->data[(r->head + r->count) % 4] = v;
  r->count++;
  return true;
}

int take(Ring* r) {
  int v = r->data[r->head];
  r->head = (r->head + 1) % 4;
  r->count--;
  return v;
}

int main() {
  if (g.head != 0 || g.count != 0 || g.data[3] != 0) return 1;
  for (int i = 1; i <= 4; i++)
    if (!put(&g, i)) return 2;
  if (put(&g, 5)) return 3;
  if (take(&g) != 1) return 4;
  if (take(&g) != 2) return 5;
  if (!put(&g, 6)) return 6;
  if (g.data[0] != 6) return 7;
  int s = 0;
  while (g.count > 0) s += take(&g);
  if (s != 3 + 4 + 6) return 8;
  int* p = &g.data[1];
  p[1] = 42;
  if (g.data[2] != 42) return 9;
  return 0;
}
