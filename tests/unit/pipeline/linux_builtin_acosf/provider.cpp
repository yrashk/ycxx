namespace {
int calls;
float received;
}

extern "C" float acosf(float value) {
  ++calls;
  received = value;
  return value + 8.0f;
}

extern "C" float apply_acosf(int*, double);

int main() {
  int evaluations = 0;
  float result = apply_acosf(&evaluations, 0.5);
  if (evaluations != 1) return 1;
  if (calls != 1) return 2;
  if (received != 0.5f) return 3;
  if (result != 8.5f) return 4;
  return 0;
}
