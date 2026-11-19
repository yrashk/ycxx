// ι (#298), from the review: a range of scalar type is one error; the body is still the loop's.
int main() {
  int s = 0;
  for (int x : 5) { //! error
    s += x;
    break;
  }
  return s;
}
