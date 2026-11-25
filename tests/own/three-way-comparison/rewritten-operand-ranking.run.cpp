// [over.match.oper]/3.4, [over.match.best.general]/2: compare the conversions in
// written operand order before preferring a candidate with non-reversed parameters.
struct BetterReversed {
    int value;
};

int operator<=>(BetterReversed lhs, long rhs) { return lhs.value < rhs ? 1 : -1; }
int operator<=>(int lhs, BetterReversed rhs) { return lhs - rhs.value; }

struct BetterForward {
    int value;
};

int operator<=>(BetterForward lhs, int rhs) { return lhs.value - rhs; }
int operator<=>(long lhs, BetterForward rhs) { return rhs.value < lhs ? -1 : 1; }

struct EqualConversions {
    int value;
};

int operator<=>(EqualConversions lhs, int rhs) { return lhs.value - rhs; }
int operator<=>(int lhs, EqualConversions rhs) { return rhs.value - lhs; }

int main() {
    BetterReversed reversed{2};
    BetterForward forward{2};
    EqualConversions tied{2};
    int rhs = 3;
    if (!(reversed < rhs) || !(reversed <= rhs) || reversed > rhs || reversed >= rhs) return 1;
    if (!(forward < rhs)) return 2;
    if (!(tied < rhs)) return 3;
    return 0;
}
