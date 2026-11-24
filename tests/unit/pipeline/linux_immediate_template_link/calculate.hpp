#ifndef YCXX_TEST_IMMEDIATE_TEMPLATE_CALCULATE_HPP
#define YCXX_TEST_IMMEDIATE_TEMPLATE_CALCULATE_HPP

// [expr.const.imm], [temp.inst]
template<int Base>
consteval int calculate(int value) {
    return Base + value;
}

#endif
