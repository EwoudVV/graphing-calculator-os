/*
 * math_parser.h - parse and evaluate math expressions
 *
 * UPDATED: now supports:
 *   - both x AND y variables (for implicit equations like circles)
 *   - trig functions: sin(), cos(), tan(), sqrt(), abs()
 *   - these use the x87 FPU's built-in hardware instructions!
 *
 * the recursive descent parser structure is the same:
 *   expression = term (('+' | '-') term)*
 *   term       = power (('*' | '/' | implicit_mult) power)*
 *   power      = unary ('^' unary)*
 *   unary      = '-' unary | atom
 *   atom       = number | 'x' | 'y' | function(expr) | '(' expr ')'
 *
 * implicit equations:
 *   if the input has '=' in it (like "x^2+y^2=4"), we treat it as
 *   an implicit equation: F(x,y) = 0 where F = left_side - right_side.
 *   we find the curve by checking where F changes sign across the graph.
 */

#ifndef MATH_PARSER_H
#define MATH_PARSER_H

#include <stdint.h>

/* === FPU math functions ===
 * the x87 FPU (Floating Point Unit) has HARDWARE implementations
 * of trig functions! we don't need Taylor series or lookup tables.
 * the CPU literally has circuits that compute sin/cos/sqrt.
 *
 * the inline assembly:
 *   flds %1   = load our float onto the FPU stack
 *   fsin      = replace top of FPU stack with sin(top)
 *   fstps %0  = store result back to our float variable
 */
static inline float my_sin(float x) {
    float result;
    __asm__ volatile ("flds %1; fsin; fstps %0" : "=m"(result) : "m"(x));
    return result;
}

static inline float my_cos(float x) {
    float result;
    __asm__ volatile ("flds %1; fcos; fstps %0" : "=m"(result) : "m"(x));
    return result;
}

/* tan doesn't have a single instruction - fptan pushes both sin and cos.
 * we use fsincos and divide. */
static inline float my_tan(float x) {
    float sin_val, cos_val;
    __asm__ volatile (
        "flds %2; fsincos; fstps %0; fstps %1"
        : "=m"(cos_val), "=m"(sin_val) : "m"(x)
    );
    if (cos_val == 0.0f) {
        volatile float zero = 0.0f;
        return zero / zero; /* NaN for undefined tan */
    }
    return sin_val / cos_val;
}

static inline float my_sqrt(float x) {
    if (x < 0.0f) {
        volatile float zero = 0.0f;
        return zero / zero; /* NaN for sqrt of negative */
    }
    float result;
    __asm__ volatile ("flds %1; fsqrt; fstps %0" : "=m"(result) : "m"(x));
    return result;
}

static inline float my_abs(float x) {
    float result;
    __asm__ volatile ("flds %1; fabs; fstps %0" : "=m"(result) : "m"(x));
    return result;
}

/* NaN helpers */
static inline float make_nan(void) {
    volatile float zero = 0.0f;
    return zero / zero;
}

static inline int is_nan(float x) {
    return x != x;
}

/* === helper functions === */

static inline int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline int is_space(char c) {
    return c == ' ';
}

static void skip_spaces(const char **s) {
    while (is_space(**s)) (*s)++;
}

static inline int starts_atom(char c) {
    return is_digit(c) || c == 'x' || c == 'y' || c == '(' || c == '.' || is_alpha(c);
}

/* simple string comparison for function names */
static int match_word(const char **s, const char *word) {
    const char *p = *s;
    while (*word) {
        if (*p != *word) return 0;
        p++; word++;
    }
    /* make sure the word ends (not a prefix of a longer word) */
    if (is_alpha(*p)) return 0;
    *s = p;
    return 1;
}

/* our own power function (integer exponents) */
static float power(float base, float exp) {
    if (exp == 0.0f) return 1.0f;

    int negative = 0;
    int n = (int)exp;
    if (n < 0) { negative = 1; n = -n; }

    float result = 1.0f;
    for (int i = 0; i < n; i++) result *= base;

    if (negative) {
        if (result == 0.0f) return make_nan();
        result = 1.0f / result;
    }
    return result;
}

/* === forward declarations === */
static float eval_expr(const char **s, float x, float y);
static float eval_term(const char **s, float x, float y);
static float eval_power(const char **s, float x, float y);
static float eval_unary(const char **s, float x, float y);
static float eval_atom(const char **s, float x, float y);

static float parse_number(const char **s) {
    float result = 0.0f;
    while (is_digit(**s)) {
        result = result * 10.0f + (**s - '0');
        (*s)++;
    }
    if (**s == '.') {
        (*s)++;
        float decimal_place = 0.1f;
        while (is_digit(**s)) {
            result += (**s - '0') * decimal_place;
            decimal_place *= 0.1f;
            (*s)++;
        }
    }
    return result;
}

/* LAYER 1: addition and subtraction */
static float eval_expr(const char **s, float x, float y) {
    float result = eval_term(s, x, y);
    while (1) {
        skip_spaces(s);
        if (**s == '+') { (*s)++; result += eval_term(s, x, y); }
        else if (**s == '-') { (*s)++; result -= eval_term(s, x, y); }
        else break;
    }
    return result;
}

/* LAYER 2: multiplication, division, implicit multiplication */
static float eval_term(const char **s, float x, float y) {
    float result = eval_power(s, x, y);
    while (1) {
        skip_spaces(s);
        if (**s == '*') { (*s)++; result *= eval_power(s, x, y); }
        else if (**s == '/') {
            (*s)++;
            float d = eval_power(s, x, y);
            if (d == 0.0f) return make_nan();
            result /= d;
        }
        else if (starts_atom(**s)) { result *= eval_power(s, x, y); }
        else break;
    }
    return result;
}

/* LAYER 3: exponentiation */
static float eval_power(const char **s, float x, float y) {
    float result = eval_unary(s, x, y);
    while (1) {
        skip_spaces(s);
        if (**s == '^') { (*s)++; result = power(result, eval_unary(s, x, y)); }
        else break;
    }
    return result;
}

/* LAYER 4: unary minus */
static float eval_unary(const char **s, float x, float y) {
    skip_spaces(s);
    if (**s == '-') { (*s)++; return -eval_unary(s, x, y); }
    return eval_atom(s, x, y);
}

/*
 * LAYER 5: atoms - numbers, variables, functions, parentheses
 *
 * NEW: recognizes function names like "sin", "cos", "tan", "sqrt", "abs"
 * and the variable 'y' for implicit equations.
 */
static float eval_atom(const char **s, float x, float y) {
    skip_spaces(s);

    if (**s == '(') {
        (*s)++;
        float result = eval_expr(s, x, y);
        skip_spaces(s);
        if (**s == ')') (*s)++;
        return result;
    }

    /* check for function names BEFORE checking for 'x'/'y'
     * so "sin" isn't parsed as "s * i * n" */
    if (match_word(s, "sin"))  { skip_spaces(s); if (**s=='(') (*s)++; float v = eval_expr(s, x, y); skip_spaces(s); if (**s==')') (*s)++; return my_sin(v); }
    if (match_word(s, "cos"))  { skip_spaces(s); if (**s=='(') (*s)++; float v = eval_expr(s, x, y); skip_spaces(s); if (**s==')') (*s)++; return my_cos(v); }
    if (match_word(s, "tan"))  { skip_spaces(s); if (**s=='(') (*s)++; float v = eval_expr(s, x, y); skip_spaces(s); if (**s==')') (*s)++; return my_tan(v); }
    if (match_word(s, "sqrt")) { skip_spaces(s); if (**s=='(') (*s)++; float v = eval_expr(s, x, y); skip_spaces(s); if (**s==')') (*s)++; return my_sqrt(v); }
    if (match_word(s, "abs"))  { skip_spaces(s); if (**s=='(') (*s)++; float v = eval_expr(s, x, y); skip_spaces(s); if (**s==')') (*s)++; return my_abs(v); }

    if (**s == 'x') { (*s)++; return x; }
    if (**s == 'y') { (*s)++; return y; }

    /* pi constant */
    if (match_word(s, "pi")) { return 3.14159265f; }

    if (is_digit(**s) || **s == '.') return parse_number(s);

    return make_nan();
}

/*
 * check if an equation string is "implicit" (contains both = and y).
 * implicit equations like "x^2+y^2=4" need special plotting.
 */
static int is_implicit_equation(const char *expr) {
    int has_equals = 0;
    int has_y = 0;
    for (int i = 0; expr[i]; i++) {
        if (expr[i] == '=') has_equals = 1;
        if (expr[i] == 'y') has_y = 1;
    }
    return has_equals && has_y;
}

/*
 * find the '=' sign in an equation and return a pointer to what's after it.
 * also gives the length of the left side.
 */
static const char *find_equals(const char *expr, int *left_len) {
    for (int i = 0; expr[i]; i++) {
        if (expr[i] == '=') {
            *left_len = i;
            return &expr[i + 1];
        }
    }
    *left_len = 0;
    return expr;
}

/*
 * evaluate an explicit equation (y = f(x) style).
 * handles: "2x+1", "x^2", "sin(x)", etc.
 */
static float evaluate(const char *expr, float x) {
    const char *s = expr;
    /* skip "y=" prefix if present */
    skip_spaces(&s);
    if (*s == 'y') {
        const char *tmp = s + 1;
        skip_spaces(&tmp);
        if (*tmp == '=') s = tmp + 1;
    }
    return eval_expr(&s, x, 0.0f);
}

/*
 * evaluate an implicit equation's F(x,y) = left - right.
 * for "x^2+y^2=4": computes (x^2+y^2) - (4) at the given (x,y).
 * the curve is where this equals zero.
 */
static float evaluate_implicit(const char *expr, float x, float y) {
    int left_len;
    const char *right_str = find_equals(expr, &left_len);

    /* make a temporary copy of the left side (on stack, no malloc!) */
    char left_buf[64];
    for (int i = 0; i < left_len && i < 63; i++) left_buf[i] = expr[i];
    left_buf[left_len < 63 ? left_len : 63] = '\0';

    const char *lp = left_buf;
    const char *rp = right_str;

    float left_val = eval_expr(&lp, x, y);
    float right_val = eval_expr(&rp, x, y);

    return left_val - right_val;
}

#endif
