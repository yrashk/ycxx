// [class.mem.general], [basic.lookup.unqual], [dcl.init.list], [temp.res]
struct Undeclared {
  int value = missing_name; //! error
};

struct BadConversion {
  int value = "wrong"; //! error
};

struct Narrowing {
  int value{1.5}; //! error
};

struct LateNamespaceName {
  int value = defined_later(); //! error
};

int defined_later() { return 1; }

template<class T> struct Nondependent {
  int value = missing_template_name; //! error
};
