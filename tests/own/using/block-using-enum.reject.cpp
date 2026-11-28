// Invalid unqualified `using enum` operands with block using-declarations and opaque enums
// ([enum.udecl]/1-2, [namespace.udecl], [basic.scope.block], #1695).
enum class Kind { Outer = 7 };

namespace Library {
  enum class Kind { Small = 4 };
  enum class AbsentAfterExit { Only = 5 };
}
namespace BadStruct {
  struct Kind {};
}
namespace BadAlias {
  using Kind = int;
}
namespace BadClassTmpl {
  template<class T> struct Kind {};
}
namespace BadAliasTmpl {
  template<class T> using Kind = T;
}
namespace BadOpaque {
  enum class Kind : int;
  enum class OnlyOpaque : int;
}
enum class GlobalOpaque : int;

void absent_name() {
  using enum Missing;  //! error
}

void struct_hides_outer_enum() {
  using BadStruct::Kind;
  using enum Kind;     //! error
}

void int_alias_hides_outer_enum() {
  using BadAlias::Kind;
  using enum Kind;     //! error
}

void class_template_hides_outer_enum() {
  using BadClassTmpl::Kind;
  using enum Kind;     //! error
}

void alias_template_hides_outer_enum() {
  using BadAliasTmpl::Kind;
  using enum Kind;     //! error
}

void opaque_hides_outer_enum() {
  using BadOpaque::Kind;
  using enum Kind;     //! error
}

void opaque_no_outer() {
  using BadOpaque::OnlyOpaque;
  using enum OnlyOpaque;  //! error
}

void global_opaque() {
  using enum GlobalOpaque;  //! error
}

void scope_exit_enumerator() {
  {
    using Library::Kind;
    using enum Kind;
    (void)Small;
  }
  (void)Small;  //! error
}

void scope_exit_using_decl() {
  {
    using Library::AbsentAfterExit;
    using enum AbsentAfterExit;
    (void)Only;
  }
  using enum AbsentAfterExit;  //! error
}

void local_class_hidden_by_struct() {
  using BadStruct::Kind;
  struct Local {
    void f() {
      using enum Kind;  //! error
    }
  };
}

namespace BadUintAlias {
  using Kind = unsigned int;
}
void local_class_hidden_by_uint_alias() {
  using BadUintAlias::Kind;
  struct Local {
    void f() {
      using enum Kind;  //! error
    }
  };
}

enum UnscopedSelf {
  SelfA = 1,
  SelfB = noexcept([] { using enum UnscopedSelf; return SelfA; }())  //! error
};

struct OuterMemberEnumHiding {
  enum class E { A = 1 };
  template<class E>
  static void run() {
    struct Local {
      static void get() {
        using enum E;  //! error
      }
    };
  }
};
