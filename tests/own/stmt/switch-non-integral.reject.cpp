// #337 (ι): the condition of a switch shall be of integral or enumeration type, or of a class
// type contextually implicitly converted to one ([stmt.switch]/2); a struct without a conversion
// function is neither.
struct Box {
  int v;
  Box(int x) : v(x) {}
};

int f(Box b) {
  switch (b) { //! error
    case 1: return 1;
  }
  return 0;
}

int main() { return f(Box(0)); }
