#pragma once

using Wide = unsigned long long;

static_assert(sizeof(bool) == 1);
static_assert(sizeof(int) == 4 && sizeof(Wide) == 8);

extern "C" {
int bool417_arguments(bool first, Wide second, Wide third, Wide fourth,
                      Wide fifth, bool sixth, bool seventh, bool eighth);
bool bool417_return(Wide source);
int bool417_promoted_returns(int from_false, int from_true);

int bool417_named(bool named, int anchor, ...);

int bool417_dirty_arguments();
bool bool417_dirty_return(int which);
int bool417_argument_bytes(bool first, Wide second, Wide third, Wide fourth,
                           Wide fifth, bool sixth, bool seventh, bool eighth);
int bool417_return_bytes();
}
