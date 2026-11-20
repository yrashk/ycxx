// [expr.type], [expr.eq], [expr.cond], [conv.ptr], [expr.static.cast]
using Fun = void (*)();
bool compare_function(void* data, Fun function) { return data == function; } //! error
struct S { int x; };
bool compare_member(void* data, int S::*member) { return data == member; } //! error
auto select_function(bool pick, void* data, Fun function) { return pick ? data : function; } //! error
void* drop_array_cv(const int (*p)[2]) { return p; } //! error
bool unrelated(int* lhs, long* rhs) { return lhs == rhs; } //! error
int* implicit_reverse(void* p) { return p; } //! error
int* drop_const(const void* p) { return static_cast<int*>(p); } //! error
using Row = int[2];
Row* drop_array_const(const void* p) { return static_cast<Row*>(p); } //! error
