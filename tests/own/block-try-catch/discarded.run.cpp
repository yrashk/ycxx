// [stmt.if] Discarding the try skips every handler and preserves the enclosing else.
template <class T>
int choose() {
  if constexpr (false)
    try {
      typename T::protected_type value;
      return value;
    } catch (typename T::exception_type value) {
      return value;
    } catch (...) {
      typename T::fallback_type value;
      return value;
    }
  else
    return 17;
}

template <int N>
int nested() {
  if constexpr (N == 1)
    return 23;
  else
    try { throw N; }
    catch (int value) { return value; }
    catch (...) { return 0; }
}

int main() {
  return choose<int>() == 17 && nested<1>() == 23 ? 0 : 1;
}
