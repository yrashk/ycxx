// #306 [class.access.general]/1: a private member class cannot be named outside its class.
class O {
  struct I { int x; };
public:
  int get() const { return I{1}.x; }
};

O::I i; //! error
