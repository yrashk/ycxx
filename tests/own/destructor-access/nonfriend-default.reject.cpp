// [class.access], [dcl.fct.default]
struct Private { private: ~Private(); };
void consume(Private = Private{});  //! error
