// [basic.scope.enum], [dcl.enum]
struct Box {
  enum State { first = Box::later, later = 4 }; //! error
};
