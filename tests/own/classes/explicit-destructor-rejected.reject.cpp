// #722: direct destruction checks the selected type, access, and deletion.
struct Other {};
struct Public { ~Public() {} };
struct Private { private: ~Private() {} };
struct Deleted { ~Deleted() = delete; };
void reject(Public& value, Private* hidden, Deleted& gone) {
  value.~Other(); // error: does not match object type
  hidden->~Private(); // error: private
  gone.~Deleted(); // error: requires a usable destructor
}
