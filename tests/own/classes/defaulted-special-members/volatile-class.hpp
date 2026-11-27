// [class.copy.ctor]: no implicit operation takes a volatile class object, so the copy and the move are
// deleted, and calls pass these by address ([class.temporary]/3).
struct Aggregate { int x; char y; };
struct Implicit { volatile Aggregate p; int k; };
struct Defaulted { Defaulted() = default; volatile Aggregate p; int k; };
struct Derived : Implicit { Derived() = default; Derived(const Derived&) = default; int d; };
struct Member { Member() = default; Member(const Member&) = default; Implicit i; int d; };

int take(Implicit);
int take(Defaulted);
int take(Derived);
int take(Member);
int peer_calls();
int peer_take(Implicit);
int peer_take(Defaulted);
int peer_take(Derived);
int peer_take(Member);
