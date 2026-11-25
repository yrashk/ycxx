// [temp.arg.nontype], [dcl.init.list]: rank stays a size type until conversion.
// Peers place the diagnostic on either the default or the point that demands it.
template<class T, bool Rank = __array_rank(T)> struct Fits {}; Fits<int[2][3]> rejected; //! error
