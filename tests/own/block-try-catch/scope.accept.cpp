// [basic.scope.block] A handler declaration can shadow a surrounding name.
// Each handler and the protected compound have distinct block scopes.
int select(int value) {
  try {
    int value = 1;
    if (value) throw value;
  } catch (int value) {
    value += 2;
    {
      int value = 3;
      if (value) return value;
    }
    return value;
  } catch (double value) {
    return value > 0.0;
  }
  return value;
}
