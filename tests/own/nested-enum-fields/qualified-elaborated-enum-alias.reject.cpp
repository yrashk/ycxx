// #761 [dcl.type.elab], [basic.lookup.elab].
struct Gauge {
  enum class Status { ready };
  using StatusAlias = Status;
};

Gauge::StatusAlias ready = Gauge::Status::ready;
enum Gauge::StatusAlias* status; //! error
