#include <ycxx/capabilities.hpp>
#include <ycxx/effects.hpp>
#include <ycxx/ownership.hpp>
#include <ycxx/ownership/variant.hpp>

namespace caps = ycxx::capabilities;
namespace fx = ycxx::effects;
namespace own = ycxx::ownership;
struct Store;
using Token = caps::token<Store, 1>;
struct Store { static Token grant() noexcept; };
Token Store::grant() noexcept { return Token(7); }
Token acquire() noexcept { return Store::grant(); }
void release(Token&& token) noexcept { token.release(); }

struct Receipt {
  own::linear_state state;
  Receipt() noexcept {}
  Receipt(Receipt&& other) noexcept
      : state(static_cast<own::linear_state&&>(other.state)) {}
};
Receipt issue() noexcept { return Receipt(); }
void close(Receipt&& receipt) noexcept { receipt.state.consume(); }

struct Read;
caps::scope_id scope;
int reads;
void read_once(const Token& token) noexcept { token.require<1>(scope); ++reads; }
void read_settings(const Token& token) noexcept { read_once(token); read_once(token); }
using Choice = own::variant<Receipt, Token>;
using Rules = own::use_policy<
    own::linear<Receipt>, own::move_transfers<Receipt>,
    own::affine<Token>, own::move_transfers<Token>, own::tracks_variant<Choice>,
    own::creates<decltype(&issue), &issue>, own::consumes<decltype(&close), &close>,
    own::creates<decltype(&acquire), &acquire>, own::consumes<decltype(&release), &release>,
    own::observes<decltype(&read_once), &read_once>, own::observes<decltype(&read_settings), &read_settings>,
    fx::trusted<decltype(&read_once), &read_once, fx::set<fx::at_most<Read, 1>>>,
    fx::bound<decltype(&read_settings), &read_settings, fx::set<fx::at_most<Read, 2>>>>;

int main(int argc, char**) {
  scope = argc <= 2 ? 7 : 9;
  auto choice = argc == 1 ? Choice(own::first_t{}, issue())
                          : Choice(own::second_t{}, acquire());
  if (choice.holds_first()) {
    close(choice.take_first());
    return reads != 0;
  }
  {
    const auto& view = choice.second();
    read_settings(view);
  }
  release(choice.take_second());
  return reads != 2;
}
