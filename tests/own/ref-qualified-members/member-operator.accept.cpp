// [over.match.oper]
struct X { int operator+(int) const &; };
struct Unrelated {};
long* operator+(Unrelated&&, int);
int member_only() { return X{} + 1; }
