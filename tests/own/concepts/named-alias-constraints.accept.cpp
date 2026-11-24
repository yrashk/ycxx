// #1227: satisfied constraints permit an explicit alias template-id to name its
// substituted type ([temp.names], [temp.alias], [temp.constr.decl]).
namespace aliases {
template<class T> concept Small = sizeof(T) < 4;
template<class U> requires Small<U> using Narrow = U;

constexpr Narrow<char> accepted = 7;
}

template<class T> using Identity = T;

int main() {
    Identity<aliases::Narrow<char>> nested = aliases::accepted;
    aliases::Narrow<char>* qualified = &nested;
    char* plain = qualified;
    return *plain == 7 ? 0 : 1;
}
