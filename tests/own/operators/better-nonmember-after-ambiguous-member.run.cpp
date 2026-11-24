// #1223, independently authored. The exact nonmember defeats both member
// conversions ([over.match.oper]/3,7–8, [over.match.best.general]/2.1,3).
// EXIT: 0
struct Number {
private:
  int operator+(long) const;
  int operator+(long long) const;
};

int operator+(const Number&, int value) { return value % 8; }

int main(int argc, char**) {
  const Number number{};
  return (number + argc) == argc % 8 ? 0 : 1;
}
