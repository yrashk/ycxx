// #133, from the step's review: a union has no reference member ([class.union.general]/2).
union U {
  int& r; //! error
  int i;
};
