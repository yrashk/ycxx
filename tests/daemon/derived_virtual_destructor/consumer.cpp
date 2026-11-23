#include "layout.hpp"
#include <cstdlib>
#include <new>

extern B* expected_base;
extern const char sentinel[];
extern unsigned base_calls, base_matches;
void delete_base(B*);

static_assert(sizeof(B) == 16 && sizeof(D) == 16);
static_assert(noexcept(static_cast<B*>(nullptr)->~B()));
static_assert(noexcept(static_cast<D*>(nullptr)->~D()));

namespace {
unsigned derived_calls = 0;
unsigned sized_deletes = 0;
unsigned unsized_deletes = 0;
void* expected_allocation = nullptr;
bool matched_delete = false;
}

void* operator new(std::size_t size) {
  if (void* address = std::malloc(size ? size : 1))
    return address;
  throw std::bad_alloc{};
}

void operator delete(void* address) noexcept {
  ++unsized_deletes;
  std::free(address);
}

void operator delete(void* address, std::size_t size) noexcept {
  ++sized_deletes;
  matched_delete = address == expected_allocation && size == 16 && base_matches == 3;
  std::free(address);
}

struct E : D {
  int value = 29;
  ~E() override;
};

E::~E() {
  if (value == 29 && name_ == sentinel && base_calls == 1)
    ++derived_calls;
}

int main() {
  // [class.dtor]: the complete-object entry destroys the base without freeing storage.
  alignas(D) unsigned char storage[sizeof(D)];
  D* complete = ::new (static_cast<void*>(storage)) D;
  expected_base = complete;
  complete->name_ = sentinel;
  complete->D::~D();
  if (base_calls != 1 || base_matches != 1 || sized_deletes || unsized_deletes)
    return 1;

  // E's destructor must call D's base-object entry.
  {
    E derived;
    expected_base = &derived;
    derived.name_ = sentinel;
  }
  if (base_calls != 2 || base_matches != 2 || derived_calls != 1 || sized_deletes || unsized_deletes)
    return 2;

  // [expr.delete]: deleting a live D via B* passes the original allocation to sized delete.
  D* allocated = new D;
  allocated->name_ = sentinel;
  expected_base = allocated;
  expected_allocation = allocated;
  delete_base(allocated);
  if (base_calls != 3 || base_matches != 3 || sized_deletes != 1 || unsized_deletes || !matched_delete)
    return 3;
}
