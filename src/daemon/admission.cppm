// When a batch applies (daemon design §4.1, §4.2): at once, but no sooner than a quantum after the
// previous one. The quantum starts at a floor, doubles whenever a root is restarted without having
// stored anything since its previous restart, and returns to the floor once every live root has.
// It chooses only the revision a request is answered at, never a value. Time is passed in, so the
// policy is tested with a simulated clock.
export module ycxx.daemon:admission;

import std;

export namespace ycxx::daemon {

class Admission {
 public:
  using Clock = std::chrono::steady_clock;
  explicit Admission(std::chrono::milliseconds floor = std::chrono::milliseconds(50)) : floor_(floor), quantum_(floor) {}

  // Whether a pending batch may apply at `now`.
  bool may_apply(Clock::time_point now) const { return !last_ || now - *last_ >= quantum_; }
  void applied(Clock::time_point now) { last_ = now; }
  // A root was stopped by a batch and is restarted; `progressed`: it stored something since its
  // previous restart.
  void restarted(bool progressed) {
    if (!progressed) quantum_ = std::min(quantum_ * 2, ceiling);
  }
  // Every live root has stored something since its last restart.
  void progressed() { quantum_ = floor_; }

  std::chrono::milliseconds quantum() const { return quantum_; }

  // A bound on the doubling, far above any cell's duration, so the quantum cannot overflow.
  static constexpr std::chrono::milliseconds ceiling{std::chrono::hours(1)};

 private:
  std::chrono::milliseconds floor_;
  std::chrono::milliseconds quantum_;
  std::optional<Clock::time_point> last_;
};

}  // namespace ycxx::daemon
