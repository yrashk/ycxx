// θ step 2 (#280), from the review: a capture follows every comma in a capture list
// ([expr.prim.lambda.capture]/1).
int main() { int x = 1; return [x, ] { return x; }(); } //! error
