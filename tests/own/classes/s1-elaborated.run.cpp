// δ s1 (#86): `struct S` and `class C` elaborated type specifiers, a pointer to a struct that is
// never defined, a class with public members, a struct first declared by an elaborated type
// specifier in a block ([dcl.type.elab], [class.name], [basic.scope.pdecl]).
// EXIT: 0
struct Opaque;
Opaque* handle = nullptr;

class Box {
public:
  int w;
  int h;
};

struct Tree {
  int key;
  struct Tree* left;
  struct Tree* right;
};

int count(struct Tree* t) {
  if (t == nullptr) return 0;
  return 1 + count(t->left) + count(t->right);
}

int main() {
  struct Tree leaves[2] = {{1, nullptr, nullptr}, {3, nullptr, nullptr}};
  struct Tree root = {2, &leaves[0], &leaves[1]};
  if (count(&root) != 3) return 1;
  if (root.left->key + root.right->key != 4) return 2;
  class Box b = {2, 3};
  Box c = b;
  if (c.w * c.h != 6) return 3;
  if (handle != nullptr) return 4;
  struct Later* lp = nullptr;
  if (lp) return 5;
  return 0;
}
