// [class.copy.assign]/1,3, [dcl.fct.def.delete]/4, [temp.inst]/2,3
template<class T>
struct Packet {
  T payload;
  Packet& operator=(const Packet&);
  Packet& operator=(Packet&&) = delete;
};

Packet<int>& copy_packet(Packet<int>& target, const Packet<int>& source) {
  return target = source;
}
