// ι (#298): the range expression is evaluated exactly once, before the first iteration, to
// initialize `auto&& __range` ([stmt.ranged]/1); a function returning a reference to a global
// container is called once, and a global array returned by reference is iterated in place
// (writes through the loop variable reach it).
// XFAIL: ycxx a namespace-scope object with a constructor needs dynamic initialization (#94, PR #371)
// EXIT: 0
struct Box {
  int v[3];
  Box() {
    v[0] = 1;
    v[1] = 2;
    v[2] = 3;
  }
  int* begin() { return v; }
  int* end() { return v + 3; }
};

Box global_box;
int box_calls = 0;
Box& the_box() {
  ++box_calls;
  return global_box;
}

int global_array[4] = {10, 20, 30, 40};
int array_calls = 0;
int (&the_array())[4] {
  ++array_calls;
  return global_array;
}

int main() {
  int sum = 0;
  for (int x : the_box()) sum += x;
  if (sum != 6) return 1;
  if (box_calls != 1) return 2;

  for (int& x : the_box()) x += 1;
  if (box_calls != 2) return 3;
  if (global_box.v[0] != 2 || global_box.v[2] != 4) return 4;

  sum = 0;
  for (int x : the_array()) sum += x;
  if (sum != 100) return 5;
  if (array_calls != 1) return 6;

  for (auto& x : the_array()) x = 0;
  if (array_calls != 2) return 7;
  if (global_array[1] != 0 || global_array[3] != 0) return 8;
  return 0;
}
