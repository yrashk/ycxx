// [temp.inst]/2,3, [over.match.best.general]/2.1, [over.ics.rank]/3.2.3,
// [dcl.fct.def.delete]/2
template<class T>
struct Packet {
  T payload;
  Packet& operator=(const Packet&);
  Packet& operator=(Packet&&) = delete;
};

Packet<int>& move_packet(Packet<int>& target, Packet<int>& source) {
  return target = static_cast<Packet<int>&&>(source); //! error
}
