// [cpp.error]/2: #warning issues a diagnostic but does not make the program ill-formed, so
// translation continues.
#warning this is only a warning
int after_warning() { return 1; }
