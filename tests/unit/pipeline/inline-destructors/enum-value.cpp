namespace {
enum class Tag : int { key = 7 };
}

enum class PublicTag : int { key = 7 };

int local_effect = 0;
int public_effect = 0;

template<Tag Key>
struct Member {
    ~Member() noexcept { local_effect += static_cast<int>(Key); }
};

template<PublicTag Key>
struct PublicMember {
    ~PublicMember() noexcept { public_effect += static_cast<int>(Key); }
};

int main() {
    {
        Member<Tag::key> local;
        PublicMember<PublicTag::key> ordinary;
    }
    if (local_effect != 7)
        return 1;
    if (public_effect != 7)
        return 2;
    return 0;
}
