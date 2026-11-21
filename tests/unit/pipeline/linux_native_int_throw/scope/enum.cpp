// A scoped enum stays a distinct type even with int as its underlying type.
enum class Signal : int { data = -5 };

extern "C" void authored_enum(Signal value) {
  throw value;
}
