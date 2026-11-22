// [class.mem.general], [basic.lookup.unqual], [basic.lookup.qual], [basic.lookup.argdep]
namespace Earlier {
struct Arg {};
int pick(long) noexcept { return 2; }
int read(const Arg&) noexcept { return 3; }
}

namespace Incomplete {
struct Arg;
extern Arg argument;
int read(const Arg&) noexcept { return 4; }
}

namespace Settings {
int pick(long) noexcept { return 1; }
constexpr int folded(long) noexcept { return 6; }

struct Record {
  int ordinary = pick(0);
  int qualified = Earlier::pick(0);
  int associated = read(Earlier::Arg{});
  int incomplete = read(Incomplete::argument);
  int enclosing = read(*this);
  int constant = folded(0);
  int body = 0;

  Record() noexcept;
  friend int read(Record&) noexcept { return 5; }
};

int pick(int) noexcept { return 7; }
constexpr int folded(int) noexcept { return 100; }
}

namespace Earlier {
int pick(int) noexcept { return 100; }
int read(Arg&&) noexcept { return 100; }
}

namespace Incomplete {
struct Arg { friend int read(Arg&) noexcept { return 100; } };
Arg argument;
}

Settings::Record::Record() noexcept { body = pick(0); }

int main() {
  Settings::Record record;
  return record.ordinary != 1 || record.qualified != 2 || record.associated != 3 || record.incomplete != 4 ||
         record.enclosing != 5 || record.constant != 6 || record.body != 7;
}
