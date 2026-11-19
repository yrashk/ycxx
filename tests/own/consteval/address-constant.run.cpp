// #101, [basic.start.static], [expr.const].
// EXIT: 8
int g = 7;
int* p = &g;
int arr[3];
int* q = arr + 1;
int main() { return *p + (q - arr); }
