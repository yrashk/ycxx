// A function with C language linkage participates in namespace ADL ([basic.lookup.argdep]).
namespace api {
struct Ticket { int number; };
extern "C" int ycxx_dispatch(Ticket&);
}
int api::ycxx_dispatch(Ticket& ticket) { return ticket.number; }
int main() {
  api::Ticket ticket{42};
  return ycxx_dispatch(ticket) - 42;
}
