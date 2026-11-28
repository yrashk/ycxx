struct x {
  int outer;
};

namespace M {
int x = 1;
int fn(int);
}  // namespace M

namespace N {
struct tag {
  int a;
};
}  // namespace N

namespace Mixed {
struct item {
  int a;
};
int item = 2;
}  // namespace Mixed

void unqualified_hidden_in_block() {
  struct x {
    int a;
  };
  using M::x;
  using T = x;  //! error
}

void unqualified_hidden_in_local_class() {
  struct x {
    int a;
  };
  using M::x;
  struct L {
    using T = x;  //! error
  };
}

int inner_tag_hides_outer_using() {
  using M::x;
  {
    struct x {
      int a;
    };
    return x;  //! error
  }
}

void tag_conflicts_with_imported_tag_after() {
  struct tag {
    int a;
  };
  using N::tag;  //! error
}

void tag_conflicts_with_imported_tag_before() {
  using N::tag;
  struct tag {  //! error
    int a;
  };
}

void tag_conflicts_with_mixed_import() {
  struct item {
    int a;
  };
  using Mixed::item;  //! error
}

void typedef_conflicts_with_using_after() {
  typedef int x;
  using M::x;  //! error
}

void typedef_conflicts_with_using_before() {
  using M::fn;
  typedef int fn;  //! error
}

void typedef_unnamed_struct_conflicts_with_using() {
  typedef struct {
    int a;
  } x;
  using M::x;  //! error
}

namespace Tpl {
template <class T>
struct CT {
  T a;
};
}  // namespace Tpl

void typedef_conflicts_with_imported_class_template() {
  using Tpl::CT;
  typedef int CT;  //! error
}

void typedef_before_imported_class_template() {
  typedef int CT;
  using Tpl::CT;  //! error
}

void elaborated_key_mismatch_and_typedef() {
  struct A;
  union A;  //! error
  union [[maybe_unused]] A;  //! error
  struct C {
    int a;
  };
  union C* p = nullptr;  //! error
  typedef int B;
  struct B;  //! error
}
