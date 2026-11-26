// [class.copy.assign]/1,3, [dcl.fct.def.delete]/4
struct Parcel {
  Parcel& operator=(const Parcel&);
  Parcel& operator=(Parcel&&) = delete;
};

Parcel& copy_parcel(Parcel& target, const Parcel& source) {
  return target = source;
}
