// #1418 [temp.inst]/3: a specialization's default member initializer is instantiated only where
// it is used, so an initializer that would be ill-formed for these arguments is not an error
// while no initialization uses it.
template <int N> struct Three { int w[N] = {1, 2, 3}; };

Three<2>* pointer = nullptr;      // no object
int size = sizeof(Three<2>);      // a complete type, no initialization
Three<2> explicit_list{{4, 5}};   // every member has an explicit clause

int main() { return explicit_list.w[1] - 5 + size - int(sizeof(int[2])); }
