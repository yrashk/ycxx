// δ s1 (#86): pointers to structs, `->`, the address of a member (`&s.x`), and `(*p).m`
// ([expr.ref], [expr.unary.op]).
// EXIT: 0
struct Counter {
  int hits;
  int misses;
};

void hit(Counter* c) { c->hits = c->hits + 1; }
void bump(int* p) { *p += 10; }

int main() {
  Counter c;
  c.hits = 0;
  c.misses = 0;
  Counter* p = &c;
  hit(p);
  hit(&c);
  if (c.hits != 2) return 1;
  p->misses = 5;
  if (c.misses != 5) return 2;
  (*p).hits = 9;
  if (p->hits != 9) return 3;
  bump(&c.misses);
  if (c.misses != 15) return 4;
  int* m = &p->hits;
  *m = 1;
  if (c.hits != 1) return 5;
  if (&c.hits != &p->hits) return 6;
  if (&(*p) != &c) return 7;
  return 0;
}
