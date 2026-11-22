// [except.spec]: prototype parameter names are in scope but are not constant values.
using Callback = int (*)(int p) noexcept(p == 0); //! error
int (*callback)(int p) noexcept(p == 0); //! error
