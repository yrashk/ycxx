// #337 (ι): a break statement shall be enclosed by an iteration or switch statement
// ([stmt.break]/1); an if statement is neither.
int f(int v) {
  if (v > 0) {
    break; //! error
  }
  return v;
}

int main() { return f(0); }
