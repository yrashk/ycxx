extern "C" float apply_acosf(int* evaluations, double input) {
  return __builtin_acosf((++*evaluations, input));
}
