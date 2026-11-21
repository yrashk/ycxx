extern "C" {

float fp201_add_f32(float a, float b) { return a + b; }
float fp201_sub_f32(float a, float b) { return a - b; }
float fp201_mul_f32(float a, float b) { return a * b; }
float fp201_div_f32(float a, float b) { return a / b; }
float fp201_neg_f32(float x) { return -x; }

double fp201_add_f64(double a, double b) { return a + b; }
double fp201_sub_f64(double a, double b) { return a - b; }
double fp201_mul_f64(double a, double b) { return a * b; }
double fp201_div_f64(double a, double b) { return a / b; }
double fp201_neg_f64(double x) { return -x; }

// Give the intermediate result its own assignment at the same width.
float fp201_round_f32(float x, float one) {
  float sum;
  sum = x + one;
  return sum - x;
}

double fp201_round_f64(double x, double one) {
  double sum;
  sum = x + one;
  return sum - x;
}

}
