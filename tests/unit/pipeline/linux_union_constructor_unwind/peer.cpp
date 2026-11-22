extern void probe();
extern int destroyed;
void external_throw() { throw 2; }
int main() {
  try {
    probe();
  } catch (int n) {
    return n != 2 || destroyed != 7;
  }
  return 2;
}
