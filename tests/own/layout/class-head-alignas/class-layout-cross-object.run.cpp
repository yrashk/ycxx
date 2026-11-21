// [basic.align], [expr.sizeof]
// REQUIRES: linux
// PEER: layout-provider.cpp
struct alignas(16) Cell { char tag; };

struct Envelope {
  char lead;
  Cell cells[2];
  char tail;
};

extern "C" int inspect(const Envelope* value);
Envelope global;

void fill(Envelope& value) {
  value.lead = 3;
  value.cells[0].tag = 5;
  value.cells[1].tag = 7;
  value.tail = 9;
}

int main() {
  Envelope automatic;
  fill(global);
  fill(automatic);
  if (int error = inspect(&global)) return error;
  if (int error = inspect(&automatic)) return 10 + error;
  return 0;
}
