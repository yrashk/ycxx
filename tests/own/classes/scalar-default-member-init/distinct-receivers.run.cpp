struct X {
  int supplied;
  int a = 5;
  int twice = this->a * 2;
};

struct Enclosing {
  int a;

  int check() {
    X x{this->a};
    if (x.supplied != 17) return 1;
    if (x.a != 5 || x.twice != 10) return 2;
    return 0;
  }
};

int main() {
  Enclosing enclosing{17};
  return enclosing.check();
}
