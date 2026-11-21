// A retained try requires exception handling even without a written throw.
// Use -fsyntax-only for front-end acceptance and -c for native diagnostics.
extern int external_call();

int protected_call() {
  try {
    return external_call();
  } catch (int result) {
    return result;
  } catch (...) {
    return -1;
  }
}
