using Size = decltype(sizeof(0));
namespace std { enum class align_val_t : Size; }
struct Tag;
extern const Tag tag;

void* operator new(Size, std::align_val_t, const Tag& = tag) noexcept;
int* before_alignment_default() { return new int; }
void* operator new(Size, std::align_val_t = static_cast<std::align_val_t>(32), const Tag&) noexcept;
int* after_alignment_default() { return new int; } //! error
