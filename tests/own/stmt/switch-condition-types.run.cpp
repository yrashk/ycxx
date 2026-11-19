// #337 (ι): switch on conditions of type int, char, unsigned, long, an unscoped and a scoped
// enumeration ([stmt.switch]/2, /5).
// EXIT: 0
enum Color { Red, Green, Blue };
enum class Shape { Circle = 10, Square = 20, Triangle = 30 };

int on_int(int v) {
  switch (v) {
    case 0: return 100;
    case 7: return 107;
    case 1000000: return 108;
  }
  return -1;
}

int on_char(char c) {
  switch (c) {
    case 'a': return 1;
    case 'z': return 26;
    case '\n': return 10;
  }
  return -1;
}

int on_unsigned(unsigned u) {
  switch (u) {
    case 0u: return 1;
    case 4000000000u: return 2;
    case 5: return 3;
  }
  return -1;
}

int on_long(long l) {
  switch (l) {
    case 3L: return 1;
    case -3L: return 2;
    case 2000000000L: return 3;
  }
  return -1;
}

int on_color(Color c) {
  switch (c) {
    case Red: return 1;
    case Green: return 2;
    case Blue: return 3;
  }
  return -1;
}

int on_shape(Shape s) {
  switch (s) {
    case Shape::Circle: return 1;
    case Shape::Square: return 2;
    case Shape::Triangle: return 3;
  }
  return -1;
}

int main() {
  if (on_int(0) != 100) return 1;
  if (on_int(7) != 107) return 2;
  if (on_int(1000000) != 108) return 3;
  if (on_int(8) != -1) return 4;
  if (on_char('a') != 1) return 5;
  if (on_char('z') != 26) return 6;
  if (on_char('\n') != 10) return 7;
  if (on_char('b') != -1) return 8;
  if (on_unsigned(0) != 1) return 9;
  if (on_unsigned(4000000000u) != 2) return 10;
  if (on_unsigned(5) != 3) return 11;
  if (on_unsigned(6) != -1) return 12;
  if (on_long(3) != 1) return 13;
  if (on_long(-3) != 2) return 14;
  if (on_long(2000000000L) != 3) return 15;
  if (on_long(4) != -1) return 16;
  if (on_color(Red) != 1) return 17;
  if (on_color(Green) != 2) return 18;
  if (on_color(Blue) != 3) return 19;
  if (on_shape(Shape::Circle) != 1) return 20;
  if (on_shape(Shape::Square) != 2) return 21;
  if (on_shape(Shape::Triangle) != 3) return 22;
  if (on_shape(static_cast<Shape>(15)) != -1) return 23;
  return 0;
}
