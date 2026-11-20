static auto triple=[](int x){return x*3;}; int main(){int (*p)(int)=triple; return p(14)-42;}
