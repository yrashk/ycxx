// [cpp.cond], [cpp.include]: a header guarded by #ifndef G / #define G / ... / #endif is
// copied once however many times it is included; an unguarded one is copied each time.
#include "inputs/guarded.h"
#include "inputs/twice.h"
#include "inputs/guarded.h"
#include "inputs/twice.h"
#include "inputs/guarded.h"
end GUARDED_MACRO;
