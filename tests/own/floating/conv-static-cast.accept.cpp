// [expr.static.cast]: static_cast between floating types, between floating and integer types,
// from a floating value to an enumeration type (through its underlying type, truncating), and from
// unscoped or scoped enumerations to floating types.
enum Color { red = 1, green = 2, blue = 3 };
enum class Level : long { low = -2, high = 7 };
enum class Small : unsigned char { z = 0, top = 255 };
static_assert(static_cast<double>(blue) == 3.0);
static_assert(static_cast<float>(green) == 2.0f);
static_assert(static_cast<double>(Level::low) == -2.0);
static_assert(static_cast<long double>(Level::high) == 7.0L);
static_assert(static_cast<double>(Small::top) == 255.0);
static_assert(static_cast<Color>(3.7) == blue);
static_assert(static_cast<Color>(1.0f) == red);
static_assert(static_cast<Level>(-2.9) == Level::low);
static_assert(static_cast<Small>(255.5) == Small::top);
static_assert(static_cast<float>(2.5) == 2.5f && static_cast<double>(2.5f) == 2.5);
static_assert(static_cast<int>(-7.5) == -7 && static_cast<double>(-7) == -7.0);
static_assert(double(blue) * 0.5 == 1.5); // a functional cast is the same conversion
static_assert(blue * 1 + 0.5 == 3.5);     // an unscoped enum promotes to int first
