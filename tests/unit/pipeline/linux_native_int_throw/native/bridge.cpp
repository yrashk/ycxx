// The return adjustment keeps this forwarding call from being a tail call.
extern "C" int authored_choice(int value);

extern "C" int authored_bridge(int value) {
  return authored_choice(value) + 9;
}
