// [cpp.cond]/2, /9: a `defined` that matches neither `defined identifier` nor
// `defined ( identifier )` is ill-formed, no diagnostic required; both oracles diagnose these
// forms, and they are cheap to detect, so ycxx is held to it too.
#if defined //! error
#endif
#if defined( //! error
#endif
#if defined(X //! error
#endif
#if defined 1 //! error
#endif
int after;
