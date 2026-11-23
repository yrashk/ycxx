// #157 [dcl.init.aggr]/10 checks each element's destructor where aggregate initialization occurs.
// XFAIL: gcc GCC accepts the initialization even though the element's destructor is private here.
struct Container;

struct Element {
  int value;
private:
  ~Element() = default;
  friend struct Container;
};

struct Container {
  Element element;
};

void create_container() {
  Container object{{42}}; //! error
}
