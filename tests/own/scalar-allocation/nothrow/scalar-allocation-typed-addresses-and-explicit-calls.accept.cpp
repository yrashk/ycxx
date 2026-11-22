using Size = decltype(sizeof(0));
namespace std { enum class align_val_t : Size; }
struct Tag;
extern const Tag tag;

void* operator new(Size, const Tag& = tag) noexcept;
void* operator new(Size, std::align_val_t, const Tag& = tag) noexcept;
void* operator new(Size, std::align_val_t = static_cast<std::align_val_t>(32), const Tag&) noexcept;

using Ordinary = void* (*)(Size);
using Tagged = void* (*)(Size, const Tag&);
using AlignedTagged = void* (*)(Size, std::align_val_t, const Tag&);

Ordinary ordinary_address() { return &::operator new; }
Tagged tagged_address() { return &::operator new; }
AlignedTagged aligned_tagged_address() { return &::operator new; }
void* tagged_call() { return ::operator new(sizeof(int), tag); }
void* aligned_tagged_call() { return ::operator new(sizeof(int), static_cast<std::align_val_t>(32), tag); }
