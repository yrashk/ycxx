enum Completed { completed_value };

enum Fixed : unsigned {
  fixed_inside = __is_constructible(Fixed)
};

enum class Opaque : unsigned;

static_assert(__is_constructible(Completed));
static_assert(__is_constructible(Fixed));
static_assert(fixed_inside == 1);
static_assert(__is_constructible(Opaque));
