// [basic.scope.param], [except.spec]
using Callback = int (*)(int p) noexcept(sizeof(p) == sizeof(int));
int (*callback)(int p) noexcept(sizeof(p) == sizeof(int));
