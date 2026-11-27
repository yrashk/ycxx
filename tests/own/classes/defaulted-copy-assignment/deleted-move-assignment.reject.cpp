// [class.copy.assign]/3, [over.match.best.general]/2.1, [over.ics.rank]/3.2.3,
// [dcl.fct.def.delete]/2
struct Parcel {
  Parcel& operator=(const Parcel&);
  Parcel& operator=(Parcel&&) = delete;
};

Parcel& move_parcel(Parcel& target, Parcel& source) {
  return target = static_cast<Parcel&&>(source); //! error
}
