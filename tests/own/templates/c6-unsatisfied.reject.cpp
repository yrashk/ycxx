// ζ c6 (#88): a call whose only candidate's constraints are not satisfied has no viable function
// ([over.match.viable]/3).
template<class T> concept Ptr = __is_pointer(T);
template<Ptr T> int f(T) { return 0; }
int main() { return f(1); } //! error
