namespace A { int a=6; static auto f=[](int x){return x*a;}; } namespace B { int a=3; static auto f=[](int x){return x+a;}; } int main(){return A::f(6)+B::f(3)-42;}
