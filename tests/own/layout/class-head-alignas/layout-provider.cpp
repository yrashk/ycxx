struct alignas(16) Cell { char tag; };

struct Envelope {
  char lead;
  Cell cells[2];
  char tail;
};

extern "C" int inspect(const Envelope* value) {
  auto base = reinterpret_cast<unsigned long>(value);
  auto first = reinterpret_cast<unsigned long>(&value->cells[0]);
  auto second = reinterpret_cast<unsigned long>(&value->cells[1]);
  auto tail = reinterpret_cast<unsigned long>(&value->tail);
  if (base % 16 != 0) return 1;
  if (first - base != 16 || second - first != 16 || tail - base != 48) return 2;
  if (value->lead != 3 || value->cells[0].tag != 5 ||
      value->cells[1].tag != 7 || value->tail != 9) return 3;
  return 0;
}
