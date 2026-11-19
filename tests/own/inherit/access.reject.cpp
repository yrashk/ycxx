// #308: access through bases ([class.access.base]): a base's public members are as accessible as
// the base itself; a private base's members and the conversion to it only inside the derived
// class; a protected member in the classes derived from its class; a `class` derives privately by
// default.
struct A { int pub; protected: int prot; private: int priv; public: int get() { return priv; } };
struct Pub : A { int f() { return pub + prot; } int g() { return priv; } };  //! error
class Priv : A { public: int f() { return pub + prot; } };
struct Prot : protected A { int f() { return pub; } };
int use(Pub& a, Priv& b, Prot& c) {
  int r = a.pub + a.get();
  r += a.prot;   //! error
  r += b.pub;    //! error
  r += c.pub;    //! error
  A* p = &b;     //! error
  return r + (p != nullptr);
}
int main() { return 0; }
