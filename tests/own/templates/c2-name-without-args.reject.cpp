// ζ c2 (#88): outside its own class, the name of a class template without a template argument
// list does not name a type. Inside the class the injected-class-name Box means Box<T>
// ([temp.local]); as a parameter type outside it, Box is a placeholder for a deduced class
// type, which is allowed only in an initializing variable declaration or an explicit type
// conversion, not in a function parameter ([temp.names], [dcl.type.class.deduct]).
template<class T>
struct Box {
  T v;
  bool same(const Box& o) const { return v == o.v; }
};

int read(const Box<int>& b) { return b.v; }
int read_bad(const Box& b); //! error
