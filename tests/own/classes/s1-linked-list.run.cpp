// δ s1 (#86): a singly linked list in a global array of nodes: arrays of structs, a struct
// with a pointer to its own type, `->` chains, a forward-declared struct used through a pointer
// before its definition ([class.mem], [basic.types.general], [expr.ref]).
// EXIT: 0
struct Node;

struct List {
  Node* head;
  int size;
};

struct Node {
  int value;
  Node* next;
};

Node pool[8];
int used = 0;

void push_front(List* l, int v) {
  Node* n = &pool[used];
  used++;
  n->value = v;
  n->next = l->head;
  l->head = n;
  l->size++;
}

int sum(List* l) {
  int s = 0;
  for (Node* n = l->head; n != nullptr; n = n->next) s += n->value;
  return s;
}

int main() {
  List l;
  l.head = nullptr;
  l.size = 0;
  for (int i = 1; i <= 5; i++) push_front(&l, i * i);
  if (l.size != 5) return 1;
  if (sum(&l) != 55) return 2;
  if (l.head->value != 25) return 3;
  if (l.head->next->next->value != 9) return 4;
  if (pool[0].value != 1 || pool[0].next != nullptr) return 5;
  if (pool[4].next != &pool[3]) return 6;
  // unlink the second node
  l.head->next = l.head->next->next;
  l.size--;
  if (sum(&l) != 39) return 7;
  return 0;
}
