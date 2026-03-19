/*
 * math_parser.h - parse and evaluate math expressions
 *
 * NOW WITH BYTECODE COMPILATION!
 *
 * the old way (slow):
 *   for each pixel:
 *     walk through "x^2+1" character by character
 *     figure out what each character means
 *     compute the result
 *
 * the new way (fast):
 *   ONCE when the user presses enter:
 *     walk through "x^2+1" and compile it to bytecode:
 *       [PUSH_X, PUSH_NUM(2), POW, PUSH_NUM(1), ADD]
 *
 *   for each pixel (thousands of times):
 *     just run the bytecode with a simple stack machine
 *     (no string scanning, no character comparisons!)
 *
 * this is literally what real compilers do:
 *   source code -> compile -> bytecode -> execute many times
 *   "x^2+1"    -> compile -> [opcodes] -> run per pixel
 *
 * the bytecode is a list of simple instructions for a "stack machine":
 *   PUSH_NUM 2  = push the number 2 onto the stack
 *   PUSH_X      = push the current x value onto the stack
 *   ADD         = pop two values, push their sum
 *   MUL         = pop two values, push their product
 *
 * example: "2x + 1" compiles to:
 *   [PUSH_NUM(2), PUSH_X, MUL, PUSH_NUM(1), ADD]
 *
 *   executing with x=3:
 *     PUSH_NUM(2)  stack: [2]
 *     PUSH_X       stack: [2, 3]
 *     MUL          stack: [6]        (popped 2 and 3, pushed 2*3)
 *     PUSH_NUM(1)  stack: [6, 1]
 *     ADD          stack: [7]        (popped 6 and 1, pushed 6+1)
 *   result: 7  correct! 2*3+1 = 7
 */

#ifndef MATH_PARSER_H
#define MATH_PARSER_H

#include <stdint.h>

/* === FPU math functions (hardware trig via x87) === */

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

static inline float my_tan(float x) {
    float sin_val, cos_val;
    __asm__ volatile (
        "flds %2; fsincos; fstps %0; fstps %1"
        : "=m"(cos_val), "=m"(sin_val) : "m"(x)
    );
    if (cos_val == 0.0f) { volatile float z = 0.0f; return z / z; }
    return sin_val / cos_val;
}

static inline float my_sqrt(float x) {
    if (x < 0.0f) { volatile float z = 0.0f; return z / z; }
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
static inline float make_nan(void) { volatile float z = 0.0f; return z / z; }
static inline int is_nan(float x) { return x != x; }

/* === helper functions === */

static inline int is_digit(char c) { return c >= '0' && c <= '9'; }
static inline int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int is_space(char c) { return c == ' '; }
static void skip_spaces(const char **s) { while (is_space(**s)) (*s)++; }
static inline int starts_atom(char c) { return is_digit(c) || c == 'x' || c == 'y' || c == '(' || c == '.' || is_alpha(c); }

static int match_word(const char **s, const char *word) {
    const char *p = *s;
    while (*word) { if (*p != *word) return 0; p++; word++; }
    if (is_alpha(*p)) return 0;
    *s = p;
    return 1;
}

static float power(float base, float exp) {
    if (exp == 0.0f) return 1.0f;
    int neg = 0; int n = (int)exp;
    if (n < 0) { neg = 1; n = -n; }
    float r = 1.0f;
    for (int i = 0; i < n; i++) r *= base;
    if (neg) { if (r == 0.0f) return make_nan(); r = 1.0f / r; }
    return r;
}

/* ============================================================
 * BYTECODE COMPILER
 * ============================================================
 * opcodes for our mini virtual machine (stack machine)
 */
#define OP_PUSH_NUM  1   /* push a constant number */
#define OP_PUSH_X    2   /* push the x variable */
#define OP_PUSH_Y    3   /* push the y variable */
#define OP_ADD       4
#define OP_SUB       5
#define OP_MUL       6
#define OP_DIV       7
#define OP_POW       8
#define OP_NEG       9   /* negate top of stack */
#define OP_SIN       10
#define OP_COS       11
#define OP_TAN       12
#define OP_SQRT      13
#define OP_ABS       14

#define MAX_OPS 128

/*
 * compiled equation = a list of opcodes + associated number values.
 *
 * ops[i] = what to do (PUSH_NUM, ADD, MUL, etc)
 * nums[i] = the number value (only used when ops[i] == OP_PUSH_NUM)
 * len = how many instructions
 */
typedef struct {
    uint8_t ops[MAX_OPS];
    float nums[MAX_OPS];
    int len;
} compiled_eq;

/* emit an opcode */
static void emit(compiled_eq *eq, uint8_t op) {
    if (eq->len < MAX_OPS) {
        eq->ops[eq->len] = op;
        eq->nums[eq->len] = 0;
        eq->len++;
    }
}

/* emit a PUSH_NUM opcode with a value */
static void emit_num(compiled_eq *eq, float val) {
    if (eq->len < MAX_OPS) {
        eq->ops[eq->len] = OP_PUSH_NUM;
        eq->nums[eq->len] = val;
        eq->len++;
    }
}

/* === compiler functions (mirror the parser structure) ===
 * instead of COMPUTING results, these EMIT bytecode instructions.
 * same recursive descent, same precedence layers. */

static void compile_expr(const char **s, compiled_eq *eq);
static void compile_term(const char **s, compiled_eq *eq);
static void compile_power(const char **s, compiled_eq *eq);
static void compile_unary(const char **s, compiled_eq *eq);
static void compile_atom(const char **s, compiled_eq *eq);

static float parse_number(const char **s) {
    float result = 0.0f;
    while (is_digit(**s)) { result = result * 10.0f + (**s - '0'); (*s)++; }
    if (**s == '.') {
        (*s)++;
        float dp = 0.1f;
        while (is_digit(**s)) { result += (**s - '0') * dp; dp *= 0.1f; (*s)++; }
    }
    return result;
}

/* LAYER 1: addition and subtraction -> emit ADD/SUB ops */
static void compile_expr(const char **s, compiled_eq *eq) {
    compile_term(s, eq);
    while (1) {
        skip_spaces(s);
        if (**s == '+')      { (*s)++; compile_term(s, eq); emit(eq, OP_ADD); }
        else if (**s == '-') { (*s)++; compile_term(s, eq); emit(eq, OP_SUB); }
        else break;
    }
}

/* LAYER 2: multiplication, division, implicit multiplication */
static void compile_term(const char **s, compiled_eq *eq) {
    compile_power(s, eq);
    while (1) {
        skip_spaces(s);
        if (**s == '*')      { (*s)++; compile_power(s, eq); emit(eq, OP_MUL); }
        else if (**s == '/') { (*s)++; compile_power(s, eq); emit(eq, OP_DIV); }
        else if (starts_atom(**s)) { compile_power(s, eq); emit(eq, OP_MUL); }
        else break;
    }
}

/* LAYER 3: exponentiation */
static void compile_power(const char **s, compiled_eq *eq) {
    compile_unary(s, eq);
    while (1) {
        skip_spaces(s);
        if (**s == '^') { (*s)++; compile_unary(s, eq); emit(eq, OP_POW); }
        else break;
    }
}

/* LAYER 4: unary minus */
static void compile_unary(const char **s, compiled_eq *eq) {
    skip_spaces(s);
    if (**s == '-') { (*s)++; compile_unary(s, eq); emit(eq, OP_NEG); }
    else compile_atom(s, eq);
}

/* LAYER 5: atoms - numbers, variables, functions, parentheses */
static void compile_atom(const char **s, compiled_eq *eq) {
    skip_spaces(s);

    if (**s == '(') {
        (*s)++;
        compile_expr(s, eq);
        skip_spaces(s);
        if (**s == ')') (*s)++;
        return;
    }

    /* functions: compile the argument, then emit the function op */
    if (match_word(s, "sin"))  { skip_spaces(s); if(**s=='(')(*s)++; compile_expr(s,eq); skip_spaces(s); if(**s==')')(*s)++; emit(eq, OP_SIN); return; }
    if (match_word(s, "cos"))  { skip_spaces(s); if(**s=='(')(*s)++; compile_expr(s,eq); skip_spaces(s); if(**s==')')(*s)++; emit(eq, OP_COS); return; }
    if (match_word(s, "tan"))  { skip_spaces(s); if(**s=='(')(*s)++; compile_expr(s,eq); skip_spaces(s); if(**s==')')(*s)++; emit(eq, OP_TAN); return; }
    if (match_word(s, "sqrt")) { skip_spaces(s); if(**s=='(')(*s)++; compile_expr(s,eq); skip_spaces(s); if(**s==')')(*s)++; emit(eq, OP_SQRT); return; }
    if (match_word(s, "abs"))  { skip_spaces(s); if(**s=='(')(*s)++; compile_expr(s,eq); skip_spaces(s); if(**s==')')(*s)++; emit(eq, OP_ABS); return; }

    if (**s == 'x') { (*s)++; emit(eq, OP_PUSH_X); return; }
    if (**s == 'y') { (*s)++; emit(eq, OP_PUSH_Y); return; }

    if (match_word(s, "pi")) { emit_num(eq, 3.14159265f); return; }

    if (is_digit(**s) || **s == '.') { emit_num(eq, parse_number(s)); return; }

    emit_num(eq, make_nan()); /* unknown token */
}

/* ============================================================
 * BYTECODE VIRTUAL MACHINE
 * ============================================================
 * runs compiled bytecode with given x and y values.
 *
 * this is a "stack machine" - every instruction either:
 *   - pushes a value onto the stack, or
 *   - pops value(s), does something, pushes the result
 *
 * at the end, the answer is the one value left on the stack.
 *
 * this is MUCH faster than re-parsing the string because:
 *   - no character-by-character scanning
 *   - no string comparisons for function names
 *   - no recursive function calls
 *   - just a tight loop with a switch statement
 */
static float eval_compiled(const compiled_eq *eq, float x, float y) {
    float stack[32];
    int sp = 0;

    for (int i = 0; i < eq->len; i++) {
        switch (eq->ops[i]) {
            case OP_PUSH_NUM: stack[sp++] = eq->nums[i]; break;
            case OP_PUSH_X:   stack[sp++] = x; break;
            case OP_PUSH_Y:   stack[sp++] = y; break;
            case OP_ADD:  if (sp >= 2) { sp--; stack[sp-1] += stack[sp]; } break;
            case OP_SUB:  if (sp >= 2) { sp--; stack[sp-1] -= stack[sp]; } break;
            case OP_MUL:  if (sp >= 2) { sp--; stack[sp-1] *= stack[sp]; } break;
            case OP_DIV:
                if (sp >= 2) {
                    sp--;
                    stack[sp-1] = (stack[sp] == 0.0f) ? make_nan() : stack[sp-1] / stack[sp];
                }
                break;
            case OP_POW:  if (sp >= 2) { sp--; stack[sp-1] = power(stack[sp-1], stack[sp]); } break;
            case OP_NEG:  if (sp >= 1) { stack[sp-1] = -stack[sp-1]; } break;
            case OP_SIN:  if (sp >= 1) { stack[sp-1] = my_sin(stack[sp-1]); } break;
            case OP_COS:  if (sp >= 1) { stack[sp-1] = my_cos(stack[sp-1]); } break;
            case OP_TAN:  if (sp >= 1) { stack[sp-1] = my_tan(stack[sp-1]); } break;
            case OP_SQRT: if (sp >= 1) { stack[sp-1] = my_sqrt(stack[sp-1]); } break;
            case OP_ABS:  if (sp >= 1) { stack[sp-1] = my_abs(stack[sp-1]); } break;
        }
    }

    return (sp > 0) ? stack[0] : make_nan();
}

/* ============================================================
 * HIGH-LEVEL API
 * ============================================================ */

/* compile an expression string into bytecode */
static void compile(const char *expr, compiled_eq *eq) {
    eq->len = 0;
    const char *s = expr;
    /* skip "y=" prefix if present */
    skip_spaces(&s);
    if (*s == 'y') {
        const char *tmp = s + 1;
        skip_spaces(&tmp);
        if (*tmp == '=') s = tmp + 1;
    }
    compile_expr(&s, eq);
}

/*
 * check if an equation is implicit.
 *
 * "y = x^2"       -> explicit (starts with y=, just strip it)
 * "x^2 + 1"       -> explicit (no = sign, treat as y = expr)
 * "x^2 + y^2 = 9" -> implicit (has = but doesn't start with y=)
 * "x = 4"         -> implicit (has = but doesn't start with y=)
 *
 * rule: if it has '=' and the left side ISN'T just 'y', it's implicit.
 */
static int is_implicit_equation(const char *expr) {
    /* check if it starts with "y =" */
    const char *s = expr;
    while (*s == ' ') s++;
    if (*s == 'y') {
        s++;
        while (*s == ' ') s++;
        if (*s == '=') return 0;  /* it's y=something, explicit */
    }

    /* any other equation with = is implicit */
    for (int i = 0; expr[i]; i++) {
        if (expr[i] == '=') return 1;
    }
    return 0;  /* no = sign, treat as explicit (y = expr) */
}

/* compile an implicit equation's left and right sides separately */
static void compile_implicit(const char *expr, compiled_eq *left, compiled_eq *right) {
    /* find the '=' */
    int eq_pos = 0;
    for (int i = 0; expr[i]; i++) {
        if (expr[i] == '=') { eq_pos = i; break; }
    }

    /* compile left side */
    char left_buf[64];
    for (int i = 0; i < eq_pos && i < 63; i++) left_buf[i] = expr[i];
    left_buf[eq_pos < 63 ? eq_pos : 63] = '\0';

    left->len = 0;
    const char *lp = left_buf;
    compile_expr(&lp, left);

    /* compile right side */
    right->len = 0;
    const char *rp = &expr[eq_pos + 1];
    compile_expr(&rp, right);
}

/* === string-parsing evaluate (used for initial animation only) === */

static float eval_expr_str(const char **s, float x, float y);
static float eval_term_str(const char **s, float x, float y);
static float eval_power_str(const char **s, float x, float y);
static float eval_unary_str(const char **s, float x, float y);
static float eval_atom_str(const char **s, float x, float y);

static float eval_expr_str(const char **s, float x, float y) {
    float result = eval_term_str(s, x, y);
    while (1) {
        skip_spaces(s);
        if (**s == '+') { (*s)++; result += eval_term_str(s, x, y); }
        else if (**s == '-') { (*s)++; result -= eval_term_str(s, x, y); }
        else break;
    }
    return result;
}

static float eval_term_str(const char **s, float x, float y) {
    float result = eval_power_str(s, x, y);
    while (1) {
        skip_spaces(s);
        if (**s == '*') { (*s)++; result *= eval_power_str(s, x, y); }
        else if (**s == '/') {
            (*s)++;
            float d = eval_power_str(s, x, y);
            if (d == 0.0f) return make_nan();
            result /= d;
        }
        else if (starts_atom(**s)) { result *= eval_power_str(s, x, y); }
        else break;
    }
    return result;
}

static float eval_power_str(const char **s, float x, float y) {
    float result = eval_unary_str(s, x, y);
    while (1) {
        skip_spaces(s);
        if (**s == '^') { (*s)++; result = power(result, eval_unary_str(s, x, y)); }
        else break;
    }
    return result;
}

static float eval_unary_str(const char **s, float x, float y) {
    skip_spaces(s);
    if (**s == '-') { (*s)++; return -eval_unary_str(s, x, y); }
    return eval_atom_str(s, x, y);
}

static float eval_atom_str(const char **s, float x, float y) {
    skip_spaces(s);
    if (**s == '(') { (*s)++; float r = eval_expr_str(s, x, y); skip_spaces(s); if (**s==')') (*s)++; return r; }
    if (match_word(s, "sin"))  { skip_spaces(s); if(**s=='(')(*s)++; float v = eval_expr_str(s,x,y); skip_spaces(s); if(**s==')')(*s)++; return my_sin(v); }
    if (match_word(s, "cos"))  { skip_spaces(s); if(**s=='(')(*s)++; float v = eval_expr_str(s,x,y); skip_spaces(s); if(**s==')')(*s)++; return my_cos(v); }
    if (match_word(s, "tan"))  { skip_spaces(s); if(**s=='(')(*s)++; float v = eval_expr_str(s,x,y); skip_spaces(s); if(**s==')')(*s)++; return my_tan(v); }
    if (match_word(s, "sqrt")) { skip_spaces(s); if(**s=='(')(*s)++; float v = eval_expr_str(s,x,y); skip_spaces(s); if(**s==')')(*s)++; return my_sqrt(v); }
    if (match_word(s, "abs"))  { skip_spaces(s); if(**s=='(')(*s)++; float v = eval_expr_str(s,x,y); skip_spaces(s); if(**s==')')(*s)++; return my_abs(v); }
    if (**s == 'x') { (*s)++; return x; }
    if (**s == 'y') { (*s)++; return y; }
    if (match_word(s, "pi")) return 3.14159265f;
    if (is_digit(**s) || **s == '.') return parse_number(s);
    return make_nan();
}

/* evaluate by parsing the string (slow, used for initial animation) */
static float evaluate(const char *expr, float x) {
    const char *s = expr;
    skip_spaces(&s);
    if (*s == 'y') { const char *t = s+1; skip_spaces(&t); if (*t == '=') s = t+1; }
    return eval_expr_str(&s, x, 0.0f);
}

static float evaluate_implicit(const char *expr, float x, float y) {
    int left_len = 0;
    for (int i = 0; expr[i]; i++) { if (expr[i] == '=') { left_len = i; break; } }
    char left_buf[64];
    for (int i = 0; i < left_len && i < 63; i++) left_buf[i] = expr[i];
    left_buf[left_len < 63 ? left_len : 63] = '\0';
    const char *lp = left_buf;
    const char *rp = &expr[left_len + 1];
    return eval_expr_str(&lp, x, y) - eval_expr_str(&rp, x, y);
}

#endif
