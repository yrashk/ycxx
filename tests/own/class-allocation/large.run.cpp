// [expr.new], [dcl.init.general]/9: value-initializing a large class zero-fills its storage
// in place; the program needs no stack for a copy of the object.
struct Big {
  unsigned char bytes[1 << 16];
  int last;
};
int main() {
  Big* filled = new Big;
  for (unsigned char& b : filled->bytes) b = 0xa5;
  filled->last = -1;
  delete filled;
  Big* b = new Big();
  int sum = b->last;
  for (unsigned char c : b->bytes) sum += c;
  delete b;
  return sum;
}
