struct A { friend long operator-(A&&); };
struct Left : A {};
struct Right : A {};
struct Value : Left, Right {};
Value make();
int operator-(Value&&);
static_assert(__is_same(decltype(-make()), int));
