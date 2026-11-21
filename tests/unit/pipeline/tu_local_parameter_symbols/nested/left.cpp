namespace {
namespace inner {
struct Record {
  enum class Kind : int { event = 5 };
  int key;
};
}
}
using Record = inner::Record;
using Kind = Record::Kind;

int choose(const Record* record) { return 10 + record->key; }
int choose(Kind kind) { return 20 + static_cast<int>(kind); }

extern "C" int check_left(int key) {
  Record record{key};
  return ::choose(&record) * 100 + ::choose(Kind::event);
}
