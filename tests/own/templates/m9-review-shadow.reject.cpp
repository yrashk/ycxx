// ζ m9 (#88, #314), from the step's review: a member template's parameter does not take a name of
// its class template's parameters ([temp.local]/6).
template<class T> struct W { template<class T> int f(); }; //! error
int main() { return 0; }
