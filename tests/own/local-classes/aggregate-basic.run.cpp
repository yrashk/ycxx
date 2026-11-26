// Local classes (#117), spec-only: a class defined in main is a local class; it is an aggregate,
// list-initialized member by member, also as array elements ([class.local], [dcl.init.aggr]).
int main() {
  struct P {
    int x;
    int y;
  };
  P p{3, 4};
  if (p.x != 3 || p.y != 4) return 1;
  P q = {5};
  if (q.x != 5 || q.y != 0) return 2;
  P arr[3] = {{1, 2}, {3, 4}, {5, 6}};
  int sum = 0;
  for (int i = 0; i < 3; ++i) sum += arr[i].x * arr[i].y;
  if (sum != 2 + 12 + 30) return 3;
  if (sizeof(arr) != 3 * sizeof(P)) return 4;
  arr[1] = p;
  if (arr[1].x != 3 || arr[1].y != 4) return 5;
  return 0;
}
