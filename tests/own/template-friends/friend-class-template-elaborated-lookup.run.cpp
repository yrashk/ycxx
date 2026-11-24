// [basic.lookup.elab]: an ordinary function nominated by a contained namespace directive
// does not hide the class template found by elaborated friend lookup.
// EXIT: 0
namespace Home {
template<class T> struct Reader {
    static int read(const T& v) { return v.secret; }
};
namespace Detail { void Reader(); }
using namespace Detail;

class Vault {
    template<class> friend struct Reader;
    int secret = 37;
};
}
int main() { return Home::Reader<Home::Vault>::read(Home::Vault{}) == 37 ? 0 : 1; }
