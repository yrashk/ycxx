// L1, the semantic level of ycxx's IR (optimization design §2; DECISIONS §4.8): the op table, the
// function body and its builder, canonical order and fingerprint, debug information, and the
// textual form for tests.
export module ycxx.ir;

export import :fingerprint;
export import :handles;
export import :ops;
export import :function;
export import :builder;
export import :identity;
export import :debug;
export import :text;
