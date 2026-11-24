#include "result.hpp"

ResultHandle forward_result(ResultObservation* observation, int value) {
  return make_result(observation, value, 301, -407, -53, -16007,
                     0x123456789abcdef0ULL);
}

int main() {
  ResultObservation named = {};
  {
    ResultHandle handle = forward_result(&named, 91);
    if (named.constructions != 1 || named.destructions != 0) {
      return 1;
    }
    if (named.constructed_at != &handle || handle.value != 91) {
      return 2;
    }
  }
  if (named.destructions != 1 || named.released_value != 91 ||
      !named.released_at_construction_address) {
    return 3;
  }

  ResultObservation discarded = {};
  make_result(&discarded, 137, 301, -407, -53, -16007, 0x123456789abcdef0ULL);
  if (discarded.constructions != 1 || discarded.destructions != 1 ||
      discarded.released_value != 137 ||
      !discarded.released_at_construction_address) {
    return 4;
  }

  ResultObservation mixed = {};
  {
    ResultPair pair = {0x123456789abcdef0ULL, 0x0fedcba987654321ULL};
    ResultHandle handle = make_pair_result(&mixed, 173, -503, 641, pair, -87, 6.5);
    if (mixed.constructions != 1 || mixed.destructions != 0 ||
        mixed.constructed_at != &handle || handle.value != 173) {
      return 5;
    }
  }
  if (mixed.destructions != 1 || mixed.released_value != 173 ||
      !mixed.released_at_construction_address) {
    return 6;
  }

  GuardObservation guarded = {};
  {
    GuardedResult handle = make_guarded_result(&guarded, 197);
    if (guarded.constructed_at != &handle || handle.value != 197 ||
        guarded.live_guards != 1 || guarded.guard_destructions != 0) {
      return 7;
    }
  }
  if (guarded.live_guards != 0 || guarded.guard_destructions != 1) {
    return 8;
  }
  return 0;
}
