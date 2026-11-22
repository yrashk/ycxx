// #761 [dcl.type.elab], [basic.lookup.elab].
struct Dial {
  enum Reading { Reading = 19 };
  enum class Scale { fine = 2 };
};

enum Dial::Reading current = Dial::Reading;
enum Dial::Scale scale = Dial::Scale::fine;
static_assert(__is_same(decltype(current), decltype(Dial::Reading)));
static_assert(__is_same(decltype(scale), Dial::Scale));

namespace lab {
struct Sensor { enum class Range { near }; };
enum Unit { metric };
}

enum lab::Sensor::Range range = lab::Sensor::Range::near;
enum ::lab::Unit unit = lab::metric;
struct lab::Sensor* sensor = nullptr;
static_assert(__is_same(decltype(range), lab::Sensor::Range));
static_assert(__is_same(decltype(unit), lab::Unit));
static_assert(__is_same(decltype(sensor), lab::Sensor*));
