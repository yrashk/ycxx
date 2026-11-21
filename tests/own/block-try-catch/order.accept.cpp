// [except.handle] Matching searches in source order.
// A later handler of the same type remains well-formed.
int choose() {
  try { throw 1; }
  catch (int first) { return first; }
  catch (int second) { return second; }
  catch (...) { return 0; }
}

// Both handlers are unnamed and empty, but only the second matches any type.
void distinct_handlers() {
  try {} catch (int) {} catch (...) {}
}
