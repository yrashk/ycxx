// #1029 [meta.unary.prop], [class.virtual], [class.abstract], [temp.arg.nontype].
template<bool Polymorphic, bool Abstract>
struct ClassFlags {
  static constexpr bool polymorphic = Polymorphic;
  static constexpr bool abstract = Abstract;
};

template<class T>
struct ClassTraits : ClassFlags<__is_polymorphic(T), __is_abstract(T)> {};

struct Plain {
  int value;
};

struct Reader {
  virtual int read() = 0;
};

struct ConcreteReader : Reader {
  int read() override;
};

static_assert(ClassTraits<Reader>::abstract);
static_assert(ClassTraits<ConcreteReader>::polymorphic);
static_assert(!ClassTraits<Plain>::polymorphic && !ClassTraits<Plain>::abstract);
static_assert(ClassTraits<const volatile Reader>::polymorphic &&
              ClassTraits<const volatile Reader>::abstract);
static_assert(ClassTraits<const ConcreteReader>::polymorphic &&
              !ClassTraits<const ConcreteReader>::abstract);
static_assert(!ClassTraits<Reader&>::polymorphic && !ClassTraits<Reader&>::abstract);
static_assert(!ClassTraits<ConcreteReader*>::polymorphic && !ClassTraits<ConcreteReader*>::abstract);
static_assert(ClassTraits<Reader>::polymorphic);
static_assert(!ClassTraits<ConcreteReader>::abstract);
