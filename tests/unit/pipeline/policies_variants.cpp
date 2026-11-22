#include <ycxx/ownership.hpp>
#include <ycxx/ownership/variant.hpp>

namespace own = ycxx::ownership;

class Receipt {
 public:
  Receipt() noexcept {}
  Receipt(Receipt&& source) noexcept
      : state_(static_cast<own::linear_state&&>(source.state_)) {}

 private:
  friend void read(const Receipt&) noexcept;
  friend void close(Receipt&&) noexcept;
  own::linear_state state_;
};

class Permit {
 public:
  Permit() noexcept {}
  Permit(Permit&& source) noexcept
      : state_(static_cast<own::affine_state&&>(source.state_)) {}

 private:
  friend void read_permit(const Permit&) noexcept;
  own::affine_state state_;
};

Receipt issue_receipt() noexcept { return Receipt(); }
Permit issue_permit() noexcept { return Permit(); }
void read(const Receipt& owner) noexcept { owner.state_.observe(); }
void read_permit(const Permit& owner) noexcept { owner.state_.observe(); }
void close(Receipt&& owner) noexcept { owner.state_.consume(); }

using Resources = own::use_policy<
    own::linear<Receipt>, own::affine<Permit>,
    own::move_transfers<Receipt>, own::move_transfers<Permit>,
    own::creates<decltype(&issue_receipt), &issue_receipt>,
    own::creates<decltype(&issue_permit), &issue_permit>,
    own::observes<decltype(&read), &read>,
    own::observes<decltype(&read_permit), &read_permit>,
    own::consumes<decltype(&close), &close>>;

using Choice = own::variant<Receipt, Permit>;

int handle(bool must_close) {
  auto choice = must_close ? Choice(own::first_t{}, issue_receipt())
                           : Choice(own::second_t{}, issue_permit());
  if (choice.holds_first()) {
    {
      const auto& view = choice.first();
      read(view);
    }
    auto receipt = choice.take_first();
    close(static_cast<Receipt&&>(receipt));
  } else {
    const auto& view = choice.second();
    read_permit(view);
  }
  return choice.holds_first();
}

int main(int argc, char**) { return handle(argc == 1); }
