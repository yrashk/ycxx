#include "const-member-layout.hpp"

int peer_containing_size(int i) { return containing_size(i); }
int peer_direct_size(int i) { return direct_size(i); }
int peer_last(const After<Contains>* a) { return a->last; }
