#include "root.hpp"

int call_key(R* root) noexcept { return root->key(); }

void destroy(R* root) noexcept { delete root; }
