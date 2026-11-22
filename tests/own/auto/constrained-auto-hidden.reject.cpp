// The block object hides the outer concept under ordinary unqualified lookup.
template<class T> concept Number = true;
int main() {
  int Number = 0;
  Number auto value = 1; //! error
  return Number;
}
