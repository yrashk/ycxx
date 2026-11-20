// [expr.eq], [expr.rel], [expr.type], [conv.qual]
static int matrix[2][3]{};
static const int readonly[2][3]{};

int main() {
  const int (*first)[3] = &matrix[0];
  volatile int (*second)[3] = &matrix[1];
  return &matrix != &readonly && &readonly != &matrix &&
         &matrix[0] == first && first == &matrix[0] &&
         first < second && second > first ? 0 : 1;
}
