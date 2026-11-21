// [class.qual], [basic.lookup.elab], [class.access.base]
struct B { using Nested = int; };
struct Public : B {};
struct Private : private B {
    using Inside = Private::B;
};

using Inherited = Public::B;
using ElaboratedOwn = struct B::B;
using ElaboratedInherited = struct Public::B;
struct HidesType : B { static int B; };
using ElaboratedHidden = struct HidesType::B;
using InheritedQualifier = HidesType::B::Nested;
using OwnQualifier = B::B::Nested;

void check_nested(InheritedQualifier* inherited, OwnQualifier* own) {
    B::Nested* exact = inherited;
    exact = own;
}

struct Mixed : Private, Public {};
using Repeated = Mixed::B;

void check(Inherited* inherited, ElaboratedOwn* own, ElaboratedInherited* elaborated,
           Repeated* repeated, Private::Inside* inside) {
    B* exact = inherited;
    exact = own;
    exact = elaborated;
    ElaboratedHidden* hidden = exact;
    exact = hidden;
    exact = repeated;
    exact = inside;
}

struct HidesWithAlias : B {
    using B = int;
    using Selected = HidesWithAlias::B;
};
HidesWithAlias::Selected selected = 1;

struct PrivateReturn : private B {
    static B make();
};
PrivateReturn::B PrivateReturn::make() { return {}; }
