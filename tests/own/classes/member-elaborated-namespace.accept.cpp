// [dcl.type.elab], [basic.scope.pdecl], [basic.scope.scope]
struct Directory {
  struct Record* first;
  Record* second;
};

Record* global_record = nullptr;

struct Record {
  int number;
};

int global_number(Directory directory) {
  return directory.second->number;
}

namespace inventory {
struct Shelf {
  class Item* select(Item*);
  Item* next;
};

Item* unqualified_item = nullptr;
}

inventory::Item* qualified_item = nullptr;

namespace inventory {
struct Item {
  int count;
};

Item* Shelf::select(Item* item) {
  return item;
}
}
