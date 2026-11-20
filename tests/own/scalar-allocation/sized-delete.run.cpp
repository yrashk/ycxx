// [expr.delete]: sized global deallocation receives the original address and object size.
// PEER: sized-delete-allocation.cpp
// PEER: sized-delete-provider.cpp
int* allocate_answer();
int allocation_provider_status();
static int evaluations;
static int* observed(int* p) {
  ++evaluations;
  return p;
}
int main() {
  int* p = allocate_answer();
  if (*p != 42) return 5;
  delete observed(p);
  int* empty = nullptr;
  delete observed(empty);
  if (evaluations != 2) return 6;
  return allocation_provider_status();
}
