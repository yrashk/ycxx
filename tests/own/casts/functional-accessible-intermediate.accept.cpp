struct InnerType { using Value = int; };
struct Public { using Inner = InnerType; };

int parens() { return Public::Inner::Value(5); }
int braces() { return Public::Inner::Value{5}; }

class Outer {
  using Inner = InnerType;
  friend int from_friend();
public:
  static int from_member();
};
int Outer::from_member() { return Outer::Inner::Value(5); }
int from_friend() { return Outer::Inner::Value{5}; }
