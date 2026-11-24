// #1223, independently authored. Each candidate has a better conversion for
// one operand ([over.match.best.general]/2.1,3, [over.ics.rank]/3.2.6,4).
struct Number {
private:
  int operator+(long) { return 1; }
};

int operator+(const Number&, int) { return 2; }

int main(int argc, char**) {
  Number number{};
  return number + argc; //! error
}
