#include "result.hpp"

ResultHandle::ResultHandle(ResultObservation* target, int payload)
    : observation(target), value(payload) {
  observation->constructed_at = this;
  observation->constructions = observation->constructions + 1;
}

ResultHandle::~ResultHandle() {
  observation->released_at_construction_address = observation->constructed_at == this;
  observation->constructed_at = nullptr;
  observation->released_value = value;
  observation->destructions = observation->destructions + 1;
}

ResultGuard::ResultGuard(GuardObservation* target) : observation(target) {
  observation->live_guards = observation->live_guards + 1;
}

ResultGuard::~ResultGuard() {
  observation->live_guards = observation->live_guards - 1;
  observation->guard_destructions = observation->guard_destructions + 1;
  observation->constructed_at = nullptr;
}

GuardedResult::GuardedResult(GuardObservation* observation, int payload)
    : guard(observation), value(payload) {
  observation->constructed_at = this;
}

GuardedResult::~GuardedResult() {}

ResultHandle make_result(ResultObservation* observation, int value, int third,
                         int fourth, signed char fifth, short sixth,
                         unsigned long long seventh) {
  // The hidden result address leaves five INTEGER argument registers.
  // The sixth and seventh explicit arguments therefore arrive on the stack.
  if (third != 301 || fourth != -407 || fifth != -53 || sixth != -16007 ||
      seventh != 0x123456789abcdef0ULL) {
    value = -1;
  }
  return ResultHandle(observation, value);
}

ResultHandle make_pair_result(ResultObservation* observation, int value,
                             int third, int fourth, ResultPair pair,
                             signed char tail, double fraction) {
  // The pair needs two INTEGER registers, so it goes on the stack.
  // The signed char can still use the remaining INTEGER register.
  if (third != -503 || fourth != 641 || pair.first != 0x123456789abcdef0ULL ||
      pair.second != 0x0fedcba987654321ULL || tail != -87 || fraction != 6.5) {
    value = -1;
  }
  return ResultHandle(observation, value);
}

GuardedResult make_guarded_result(GuardObservation* observation, int value) {
  return GuardedResult(observation, value);
}
