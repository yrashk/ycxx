// Object and function types select distinct pointer overloads (#209).
// EXIT: 0
template<bool> struct Tag {};
template<class T> using Signature = Tag<__is_object(T)>;

struct Incomplete;
using Function = int(short) noexcept;

int select(Signature<Incomplete>*) { return 1; }
int select(Signature<Function>*) { return 2; }

int main() {
    Tag<true>* object = nullptr;
    Tag<false>* nonobject = nullptr;
    if (select(object) != 1)
        return 1;
    if (select(nonobject) != 2)
        return 2;
    return 0;
}
