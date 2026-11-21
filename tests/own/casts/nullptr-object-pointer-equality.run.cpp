using N = decltype(nullptr);

int main() {
  int object = 0;
  int* nonnull = &object;
  int* null = nullptr;
  N n = nullptr;

  if (!(null == n) || n != null)
    return 1;
  if (n == nonnull || !(nonnull != n))
    return 2;

  const N constant = nullptr;
  volatile N observed = nullptr;
  if (!(constant == null) || observed != null)
    return 3;
  if (nonnull == observed || !(constant != nonnull))
    return 4;
}
