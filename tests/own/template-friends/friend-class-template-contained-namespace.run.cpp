// [dcl.type.elab]/4 and [namespace.udir]: a directive that nominates a contained namespace
// contributes at the same namespace level. Inline namespaces contribute there too.
// XFAIL: gcc GCC 16 excludes contained using-directives and inline-namespace members here.
// EXIT: 0
namespace Home {
namespace Detail {
template<class> struct Reader;
}
using namespace Detail;
using namespace Detail;

inline namespace Version {
template<class> struct VersionReader;
}

class Vault {
    template<class> friend struct Reader;
    template<class> friend struct VersionReader;
    int secret = 37;
};

namespace Detail {
template<class T>
struct Reader {
    static int read(const Vault& v) { return v.secret; }
};
}
inline namespace Version {
template<class T>
struct VersionReader {
    static int read(const Vault& v) { return v.secret; }
};
}
}

int main() {
    return Home::Detail::Reader<int>::read(Home::Vault{}) == 37 &&
           Home::VersionReader<int>::read(Home::Vault{}) == 37 ? 0 : 1;
}
