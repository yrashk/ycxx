#include "profile.hpp"

// [temp.expl.spec], [basic.def.odr]: a non-inline explicit definition is not mergeable.
template<> unsigned Profile<2u>::limit = 2u;
int main() { return Profile<2u>::limit != 2u; }
