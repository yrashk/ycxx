// #1172: undefined __int128 operations are not constant expressions ([expr.const], [expr.pre]/4,
// [expr.mul]/4, [expr.shift]/1, [conv.fpint]/1).
// FLAGS: -Wno-pedantic
constexpr unsigned __int128 umax = ~(unsigned __int128)0;
constexpr __int128 imax = (__int128)(umax >> 1);
constexpr __int128 a = imax + 1; //! error
constexpr __int128 b = imax * 2; //! error
constexpr __int128 c = (-imax - 1) / -1; //! error
constexpr unsigned __int128 d = (unsigned __int128)1 << 128; //! error
constexpr __int128 e = -imax - 2; //! error
constexpr __int128 f = (__int128)0x1p127; //! error
constexpr unsigned __int128 g = umax / (umax - umax); //! error
