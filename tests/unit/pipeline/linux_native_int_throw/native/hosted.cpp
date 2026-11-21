// [except.handle] The hosted compiler owns handlers, cleanups and its exception runtime.
extern "C" int authored_choice(int value);
extern "C" int authored_bridge(int value);
extern "C" int authored_operand(int mode);

struct ForeignError {
  int code;
};

static int operand_calls;
static int destroyed;

struct Cleanup {
  ~Cleanup() { ++destroyed; }
};

extern "C" int hosted_operand(int mode) {
  ++operand_calls;
  if (mode == 1) throw ForeignError{87};
  return -125;
}

int main() {
  {
    Cleanup guard;
    if (authored_choice(0) != 41 || authored_bridge(0) != 50) return 10;
    if (destroyed != 0 || operand_calls != 0) return 11;
  }
  if (destroyed != 1) return 12;

  try {
    Cleanup guard;
    authored_bridge(-73);
    return 20;
  } catch (unsigned) {
    return 21;
  } catch (int payload) {
    if (payload != -73 || destroyed != 2 || operand_calls != 0) return 22;
  } catch (...) {
    return 23;
  }

  try {
    Cleanup guard;
    authored_operand(0);
    return 30;
  } catch (unsigned) {
    return 31;
  } catch (int payload) {
    if (payload != -125 || destroyed != 3 || operand_calls != 1) return 32;
  } catch (...) {
    return 33;
  }

  try {
    Cleanup guard;
    authored_operand(1);
    return 40;
  } catch (int) {
    return 41;
  } catch (const ForeignError& original) {
    if (original.code != 87 || destroyed != 4 || operand_calls != 2) return 42;
  } catch (...) {
    return 43;
  }

  return destroyed == 4 && operand_calls == 2 ? 0 : 50;
}
