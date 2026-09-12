/**
 * calc_engine.h — 科学计算器 表达式求值引擎 (自研, 轻量).
 *
 * 支持: 数字(小数/科学计数) 负号, 运算符 + - * / % ^ !, 括号,
 *       隐式乘法(2pi, 3(4)), 常量 pi/e/Ans,
 *       函数 sin cos tan asin acos atan sinh cosh tanh ln log log2 sqrt exp abs floor ceil.
 * 采用 词法 + 调车场(shunting-yard)转后缀 + 后缀求值, double 精度.
 */
#ifndef CALC_ENGINE_H
#define CALC_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 求值数学表达式.
 * expr: 表达式字符串; ans: 上一结果(供 Ans); 
 * 返回 0=成功(结果写 *out); -1=语法错误; -2=域/数学错误(非数字/求值时). */
int calc_eval(const char *expr, double ans, double *out);

#ifdef __cplusplus
}
#endif

#endif /* CALC_ENGINE_H */