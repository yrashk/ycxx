// δ s3 (#86): a temporary is destroyed at the end of the full-expression that creates it, after
// the call it is passed to has returned; `S s = S(…)` creates no extra temporary; a temporary
// bound to a local const reference lives as long as the reference
// ([class.temporary], [intro.execution] full-expression, [dcl.init.general]).
// EXIT: 0
int alive = 0;
int made = 0;

struct Token {
  int v;
  Token(int x) : v(x) { ++alive; ++made; }
  ~Token() { --alive; }
  int value() const { return v; }
};

int seen_alive = -1;

int use(const Token& t) {
  seen_alive = alive;
  return t.value();
}

int main() {
  int r = use(Token(5)) + use(Token(6));
  if (r != 11) return 1;
  if (seen_alive < 1) return 2;  // the argument was alive during the call
  if (alive != 0) return 3;      // both gone by the end of the statement
  if (made != 2) return 4;

  if (Token(7).value() != 7) return 5;
  if (alive != 0) return 6;

  made = 0;
  {
    Token t = Token(8);  // initialized directly ([dcl.init.general]): one object
    if (made != 1 || alive != 1) return 7;
    const Token& ref = Token(9);
    if (alive != 2) return 8;
    if (ref.value() != 9) return 9;
  }
  if (alive != 0) return 10;
  return 0;
}
