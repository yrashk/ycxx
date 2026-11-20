// [over.ics.list], [over.ics.rank] (#231)
struct P {
    int first;
    int second;
};

struct AggregateResult;
struct ScalarResult;

AggregateResult* choose(P);
ScalarResult* choose(long);

AggregateResult* verify_two(decltype(choose({1, 2})) result) { return result; }
ScalarResult* verify_one(decltype(choose({1})) result) { return result; }
ScalarResult* verify_empty(decltype(choose({})) result) { return result; }
