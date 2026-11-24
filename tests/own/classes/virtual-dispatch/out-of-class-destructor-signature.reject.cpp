// #309 [class.dtor]/1: a destructor takes no parameters and has no cv-qualifier.
struct Root {
  virtual ~Root();
  virtual int f();
};

Root::~Root(int) {} //! error
