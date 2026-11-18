// #133: static_cast does not cast away constness ([expr.static.cast]/1, [expr.const.cast]).
const int c = 1;
int* r = static_cast<int*>(&c); //! error
