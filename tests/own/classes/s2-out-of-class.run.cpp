// δ s2 (#86): member functions declared in the class and defined out of class with a qualified
// name, implicit member access and calls in out-of-class definitions, a class with private
// data and public functions ([class.mfct], [class.mfct.non.static], [class.access]).
// EXIT: 0
class Account {
  long balance;
  int ops;

public:
  void open(long initial);
  bool withdraw(long amount);
  void deposit(long amount);
  long get() const;
  int count() const;
};

void Account::open(long initial) {
  balance = initial;
  ops = 0;
}

void Account::deposit(long amount) {
  balance += amount;
  ++ops;
}

bool Account::withdraw(long amount) {
  if (amount > get()) return false;
  balance -= amount;
  ops++;
  return true;
}

long Account::get() const { return balance; }
int Account::count() const { return ops; }

int main() {
  Account a;
  a.open(100);
  a.deposit(50);
  if (!a.withdraw(120)) return 1;
  if (a.withdraw(31)) return 2;
  if (a.get() != 30) return 3;
  if (a.count() != 2) return 4;
  Account* p = &a;
  p->deposit(5);
  if (p->get() != 35 || p->count() != 3) return 5;
  return 0;
}
