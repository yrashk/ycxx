// ι (#291): a static block variable declared constinit must have static initialization
// ([dcl.constinit]/2); an initializer calling a non-constexpr function is not a constant
// initializer.
int f() { return 1; }
constexpr int g() { return 2; }

int h() {
  constinit static int ok = g();
  constinit static int bad = f(); //! error
  return ok + bad;
}

int main() { return h(); }
