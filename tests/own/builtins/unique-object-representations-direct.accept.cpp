// #1062 [meta.unary.prop], [basic.types.general], [basic.fundamental].
using Bytes = const unsigned char[2][3];
using OpenBytes = const volatile signed char[][3];
using OpenFloats = const float[][2];
using Function = int(double);

static_assert(__has_unique_object_representations(char));
static_assert(__has_unique_object_representations(signed char));
static_assert(__has_unique_object_representations(volatile unsigned char));
static_assert(__has_unique_object_representations(const char8_t));
static_assert(__has_unique_object_representations(Bytes));
static_assert(__has_unique_object_representations(OpenBytes));

static_assert(!__has_unique_object_representations(float));
static_assert(!__has_unique_object_representations(double));
static_assert(!__has_unique_object_representations(OpenFloats));
static_assert(!__has_unique_object_representations(Function));
static_assert(!__has_unique_object_representations(char&));
static_assert(!__has_unique_object_representations(const volatile void));
