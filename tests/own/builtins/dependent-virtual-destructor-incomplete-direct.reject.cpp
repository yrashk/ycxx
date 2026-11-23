// #1059 [meta.unary.prop] requires completeness at the written query.
struct Channel;
constexpr bool early = __has_virtual_destructor(Channel); //! error
struct Channel {
  virtual ~Channel() noexcept;
};
