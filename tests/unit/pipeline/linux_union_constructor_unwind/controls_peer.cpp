extern void probe_controls();
extern int owner_destroyed;
extern int union_destroyed;
int mode;
int initializer_calls;
int body_calls;
int initialize_payload() {
  ++initializer_calls;
  if (mode == 1) throw 31;
  return 7;
}
void after_initialize() {
  ++body_calls;
  if (mode == 2) throw 43;
}
int main() {
  mode = 0;
  probe_controls();
  if (initializer_calls != 1 || body_calls != 1 || union_destroyed != 1 || owner_destroyed != 0)
    return 1;
  mode = 1;
  try {
    probe_controls();
    return 2;
  } catch (int n) {
    if (n != 31 || initializer_calls != 2 || body_calls != 1 ||
        union_destroyed != 1 || owner_destroyed != 0)
      return 3;
  }
  mode = 2;
  try {
    probe_controls();
    return 4;
  } catch (int n) {
    return n != 43 || initializer_calls != 3 || body_calls != 2 ||
           union_destroyed != 1 || owner_destroyed != 7;
  }
}
