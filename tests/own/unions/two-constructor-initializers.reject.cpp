// #203: [class.base.init] forbids initializing two union members in a constructor.
union Choice {
  int first;
  int second;
  Choice() : first(1), second(2) {} //! error
};
