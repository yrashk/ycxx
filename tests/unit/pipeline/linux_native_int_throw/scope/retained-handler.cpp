// The int throw has a handler in the authored function.
extern "C" int authored_local_handler(int value) {
  try {
    throw value;
  } catch (int payload) {
    return payload;
  }
}
