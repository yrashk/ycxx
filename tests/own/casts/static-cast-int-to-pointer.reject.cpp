// #133: static_cast does not convert an integer to a pointer ([expr.static.cast]).
int* p = static_cast<int*>(1L); //! error
