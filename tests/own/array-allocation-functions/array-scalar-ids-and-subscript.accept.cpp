// [over.oper], [expr.sub] Array allocation and a subscript operator have distinct ids.
using Count = decltype(sizeof(0));
using Allocate = void* (*)(Count);

Allocate array_address() { return &::operator new[]; }
Allocate scalar_address() { return &::operator new; }

struct Indexed {
  int operator[](int offset) const { return offset + 1; }
};

int read(const int* pointer, int index) {
  return pointer[index];
}
