int exception_source(int);
struct Plain { int value; };

int via_direct() { return exception_source(7); }
int via_explicit_temporary() { return Plain{exception_source(7)}.value; }
int via_literal_temporary() { return Plain{19}.value; }
