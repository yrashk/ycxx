// [basic.lookup.argdep]
template<class T> struct Holder { int value; };
namespace N {
struct Tag {};
int score(const Holder<Tag>& h) { return h.value + 1; }
}
int main() { Holder<N::Tag> value{41}; return score(value) == 42 ? 0 : 1; }
