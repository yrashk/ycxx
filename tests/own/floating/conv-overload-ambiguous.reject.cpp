// [over.ics.rank], [over.match.best], [conv.double], [conv.fpint]: only float to double is a
// promotion; double to float and double to long double are both conversions, as are int to float
// and int to double, so each call below is ambiguous.
int a(float);
int a(long double);
int b(float);
int b(double);
int c(int);
int c(float);
int d(long);
int d(double);
int use() {
  int r = 0;
  r += a(1.0); //! error
  r += b(1); //! error
  r += b(1.0L); //! error
  r += c(1.0); //! error
  r += d(1); //! error
  r += a(1.0f) + b(1.0f) + c(1.0f) + d(1.0);
  return r;
}
