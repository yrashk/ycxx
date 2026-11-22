// A bare throw is well-formed and rethrows the currently handled exception when one exists.
extern "C" void authored_rethrow() {
  throw;
}
