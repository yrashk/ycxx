using R = int;
using Arg = int;
int target(int (*function)(Arg*)) { return function(nullptr); }
int check() {
  int (*callback)(typename ::R (Arg*)) = target;
  return callback(nullptr);
}
