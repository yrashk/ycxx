// ζ c2 (#88): a specialization has exactly the members its class template declares; naming a
// member Box<int> does not have is diagnosed where it is used, after the specialization is
// instantiated, and declaring the class with that argument alone is fine
// ([temp.inst], [expr.ref], [class.member.lookup]).
template<class T>
struct Box {
  T v;
  T get() const { return v; }
};

int main() {
  Box<int> b{1};
  int ok = b.get();
  return ok + b.size(); //! error
}
