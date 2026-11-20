// [expr.type], [conv.qual]
using Row = int[3];
using ConstRow = const int[3];
static Row row{};
static ConstRow readonly{};
static const int wrong_bound[2]{};
static const long wrong_element[3]{};
static Row* slot = &row;

Row* drops_const = &readonly; //! error
ConstRow** unsafe = &slot; //! error
bool mismatched_bound = &row == &wrong_bound; //! error
bool mismatched_element = &row == &wrong_element; //! error
