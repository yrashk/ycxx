// #761 [basic.lookup.qual], [namespace.qual], [basic.lookup.elab].
namespace imported {
struct Channel { enum class Mode { receive }; };
}

namespace local {
using namespace imported;
int Channel;
}

enum local::Channel::Mode mode = imported::Channel::Mode::receive;
static_assert(__is_same(decltype(mode), imported::Channel::Mode));
static_assert(__is_same(decltype(local::Channel), int));
