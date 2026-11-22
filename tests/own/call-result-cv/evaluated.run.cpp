// #679 [expr.type], [dcl.init.ref], [over.ics.rank].
// EXIT: 0
struct Ticket { int number; };

Ticket ordinary_ticket() { return {17}; }
const Ticket reserved_ticket() { return {23}; }

int fare(Ticket&& ticket) { return ticket.number + 100; }
int fare(const Ticket& ticket) { return ticket.number + 200; }

int main() {
  if (fare(ordinary_ticket()) != 117) return 1;
  if (fare(reserved_ticket()) != 223) return 2;
  return 0;
}
