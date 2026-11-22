// [dcl.enum], [basic.scope.scope], [class.mem.general]
struct MethodFirst {
  int active() const { return 17; }
  enum State { active }; //! error
};

struct FieldBeforeTag {
  int active;
  enum class active { idle };
  enum State { active }; //! error
};

struct AliasFirst {
  using active = int;
  enum State { active }; //! error
};

struct EnumeratorFirst {
  enum State { active };
  enum Mode { active }; //! error
};

struct Named {
  enum State { Named }; //! error
};

template<class T>
struct TemplateFirst {
  int active() const { return 17; }
  enum State { active }; //! error
};

TemplateFirst<int> instance;
