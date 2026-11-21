// EXIT: 0
namespace Outer {
template<class T> inline constexpr bool flag = false;
template<> inline constexpr bool flag<int> = true;
}

template<bool> struct Tag {};
template<class T> using Signature = Tag<!::Outer::flag<T>>;

int choose(Signature<int>*) { return 19; }
int choose(Signature<long>*) { return 23; }

int main() {
    int from_specialization = choose(static_cast<Tag<false>*>(nullptr));
    int from_primary = choose(static_cast<Tag<true>*>(nullptr));
    return from_specialization == 19 && from_primary == 23 ? 0 : 1;
}
