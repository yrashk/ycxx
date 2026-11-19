// ζ p4 (#88): a class template's pack is its last parameter ([temp.param]).
template<class... Ts, class U> struct X; //! error
template<class U, class... Ts> struct Y;
int main() { return 0; }
