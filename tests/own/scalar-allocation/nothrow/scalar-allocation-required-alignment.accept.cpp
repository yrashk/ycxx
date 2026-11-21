using Size = decltype(sizeof(0));
namespace std { enum class align_val_t : Size; }
struct Tag;
extern const Tag tag;
void* operator new(Size, std::align_val_t, const Tag& = tag) noexcept;

int* allocate() { return new int; }
