// [basic.lookup.elab]: an ordinary function nominated by a contained namespace directive
// does not hide the class template found by elaborated friend lookup.
// EXIT: 0
namespace Home {
template<class> struct Reader;
namespace Detail {
void Reader();
}
using namespace Detail;

class Vault {
    template<class> friend struct Reader;
    int secret = 37;
};
}

template<class T>
struct Home::Reader {
    static int read(const Home::Vault& v) { return v.secret; }
};

int main() { return Home::Reader<int>::read(Home::Vault{}) == 37 ? 0 : 1; }
