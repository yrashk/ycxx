// A user header: its warning is reported.
#pragma once
inline int user() { return 'ef' == 0 ? 0 : 2; } //! warning
