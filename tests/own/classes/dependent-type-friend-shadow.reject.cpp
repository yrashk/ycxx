// [temp.local], [class.access]: the found type decides which class has access.
struct BaseIssuer;
struct SubstitutedIssuer;

struct Base {
  using Authority = BaseIssuer;
};

template<class Authority>
class Vault : public Base {
  friend Authority;
  int value = 3;
};

struct BaseIssuer {
  static int read(const Vault<SubstitutedIssuer>& vault) { return vault.value; }
};

struct SubstitutedIssuer {
  static int read(const Vault<SubstitutedIssuer>& vault) { return vault.value; }  //! error
};
