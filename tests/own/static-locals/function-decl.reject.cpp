// ι (#291): static applies to block variables, but a function declared at block scope cannot
// be static ([dcl.stc]: no static function declarations within a block).
int g() { return 3; }

int h() {
  static int ok = 1;
  static int g(); //! error
  return ok + g();
}

int main() { return h(); }
