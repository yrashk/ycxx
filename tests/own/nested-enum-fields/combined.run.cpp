// [class.mem.general], [dcl.enum], [dcl.meaning]
struct Record {
    char lead;
    enum Kind { first = 3, last = first + 4 } a, b;
    int tail;
};

int main() {
    Record r;
    Record::Kind k = Record::last;
    r.lead = 1;
    r.a = Record::first;
    r.b = k;
    r.tail = 29;
    if (r.lead != 1 || r.a != 3 || r.b != 7 || r.tail != 29)
        return 1;
    if (reinterpret_cast<unsigned long>(&r.a) - reinterpret_cast<unsigned long>(&r) != 4 || reinterpret_cast<unsigned long>(&r.b) - reinterpret_cast<unsigned long>(&r) != 8)
        return 2;
    return reinterpret_cast<unsigned long>(&r.tail) - reinterpret_cast<unsigned long>(&r) != 12 || sizeof(Record) != 16;
}
