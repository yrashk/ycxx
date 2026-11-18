// ζ c2 (#88): a member function whose body is ill-formed for this argument is harmless until it is
// used; calling it instantiates the definition, and the error is in the body: int has no member
// size ([temp.inst]/5, [temp.res], [expr.ref]). get, also instantiated, is fine.
template<class T>
struct Box {
  T v;
  T get() const { return v; }
  int length() const {
    return v.size(); //! error
  }
};

int main() {
  Box<int> b{1};
  return b.get() + b.length();
}
