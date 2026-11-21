// The live automatic object requires cleanup when the exception exits this frame.
struct Pending {
  int data;
  ~Pending();
};

extern "C" void authored_active_cleanup(int value) {
  Pending pending{value};
  throw value;
}
