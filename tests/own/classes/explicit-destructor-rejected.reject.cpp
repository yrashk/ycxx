// #722: direct destruction checks the selected type, access, and deletion.
struct Other {};
struct Public { ~Public() {} };
struct Private { private: ~Private() {} };
struct Deleted { ~Deleted() = delete; };
struct ContainsDeleted { Deleted member; };
void reject(Public& value, Private* hidden, Deleted& gone, ContainsDeleted& also_gone) {
  value.~Other(); //! error
  hidden->~Private(); //! error
  gone.~Deleted(); //! error
  also_gone.~ContainsDeleted(); //! error
}
