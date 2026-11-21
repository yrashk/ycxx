// The resolved class is the ordinary base subobject used for layout and conversions (#308).
// EXIT: 0
struct Payload { int size; };
template<class T> struct Holder : T { int tag; };

int main() {
  Holder<Payload> h{{7}, 11};
  Payload* pointer = &h;
  Payload& reference = h;
  return pointer == &reference && pointer->size == 7 && h.tag == 11 ? 0 : 1;
}
