#ifndef YCXX_TEST_LINUX_NONTRIVIAL_RESULT_HPP
#define YCXX_TEST_LINUX_NONTRIVIAL_RESULT_HPP

struct ResultHandle;

struct ResultObservation {
  const ResultHandle* constructed_at;
  int constructions;
  int destructions;
  int released_value;
  bool released_at_construction_address;
};

struct ResultHandle {
  ResultObservation* observation;
  int value;

  ResultHandle(ResultObservation* target, int payload);

  ResultHandle(const ResultHandle&) = delete;
  ResultHandle(ResultHandle&&) = delete;

  ~ResultHandle();
};

ResultHandle make_result(ResultObservation* observation, int value, int third,
                         int fourth, signed char fifth, short sixth,
                         unsigned long long seventh);

struct ResultPair {
  unsigned long long first;
  unsigned long long second;
};

ResultHandle make_pair_result(ResultObservation* observation, int value,
                             int third, int fourth, ResultPair pair,
                             signed char tail, double fraction);

struct GuardedResult;

struct GuardObservation {
  const GuardedResult* constructed_at;
  int live_guards;
  int guard_destructions;
};

struct ResultGuard {
  GuardObservation* observation;

  ResultGuard(GuardObservation* target);
  ~ResultGuard();
};

struct GuardedResult {
  ResultGuard guard;
  int value;

  GuardedResult(GuardObservation* observation, int payload);
  ~GuardedResult();
};

GuardedResult make_guarded_result(GuardObservation* observation, int value);

#endif
