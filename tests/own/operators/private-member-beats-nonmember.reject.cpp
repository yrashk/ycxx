// #1223, independently authored. The exact member wins and access is
// checked afterward ([over.match.best.general]/2.1,3, [class.access.general]/4–5).
struct Number {
private:
  int operator+(int) const { return 1; }
};

int operator+(const Number&, long) { return 2; }

int main(int argc, char**) {
  const Number number{};
  return number + argc; //! error
}
