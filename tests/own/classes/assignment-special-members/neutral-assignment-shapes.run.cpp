struct TemplateAssigned {
    int value;

    template<class Tag = void>
    void operator=(TemplateAssigned&&);
};

struct IntegerAssigned {
    int value;
    void operator=(int);
};

int main() {
    TemplateAssigned template_source;
    template_source.value = 37;
    const TemplateAssigned& template_lvalue = template_source;
    TemplateAssigned copied_template(template_lvalue);

    IntegerAssigned integer_source;
    integer_source.value = 53;
    const IntegerAssigned& integer_lvalue = integer_source;
    IntegerAssigned copied_integer(integer_lvalue);
    return copied_template.value == 37 && copied_integer.value == 53 ? 0 : 1;
}
