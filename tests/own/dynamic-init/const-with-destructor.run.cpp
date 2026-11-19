// ι (#94), from the review: a const object constant-initialized but with a destructor is still
// written at exit, so it is not placed in read-only memory ([basic.start.term], [class.dtor]).
// STDOUT: 1dd
// EXIT: 0
extern "C" int putchar(int);
struct S {
  int x;
  ~S() {
    int was = x;
    x = 0;
    putchar('d');
    if (was == 1) putchar('\n');  // `s`, destroyed last
  }
};
const S s = {1};
const S arr[1] = {{2}};
int main() {
  putchar('0' + s.x);
  return arr[0].x == 2 ? 0 : 1;
}
