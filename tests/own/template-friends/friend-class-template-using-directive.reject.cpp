// [dcl.type.elab]/4 and [namespace.udir]: the directive puts Other::Reader at the common
// ancestor, outside the namespace where unqualified friend lookup stops.
namespace Other {
template<class> struct Reader;
}

namespace Home {
using namespace Other;
class Vault {
    template<class> friend struct Reader;
    int secret = 37;
};

template<class T>
struct Reader {
    static int read(const Vault& v) { return v.secret; }
};
}

namespace Other {
template<class T>
struct Reader {
    static int read(const Home::Vault& v) { return v.secret; } //! error
};
}

int main() {
    return Home::Reader<int>::read(Home::Vault{}) + Other::Reader<int>::read(Home::Vault{});
}
