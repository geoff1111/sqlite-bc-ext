<img src="bclite-logo.png" alt="bclite-logo" width="25%">

# BC-Lite Programming Manual

BC-Lite is a small, Tcl-inspired language for writing arbitrary-precision decimal procedures for the SQLite BC extension. It is designed to be human readable, and writable either by hand or with the assistance of LLM technology. It is not a general-purpose scripting language -- every runtime value is numeric, arithmetic is decimal and arbitrary precision, and programs are compiled to the connection-local BC virtual machine.

## 1. The basic model

A BC-Lite source string is a **compilation unit** containing one or more `proc` definitions.

```tcl
proc square {x} {
    return [mul $x $x]
}

proc square_plus_one {x} {
    return [add [square $x] #1]
}
```

A compilation unit is compiled as one transaction:

- procedures in the unit may call procedures defined later in the same unit;
- unresolved calls create source-procedure placeholders while compiling;
- every source placeholder must be resolved before the unit finishes;
- if any procedure fails to compile, the whole unit is rolled back;
- procedures successfully installed by an earlier compilation unit remain available;
- procedure names and arities are fixed once installed.

Numeric constants are validated and imported into the connection's BCL context at compile time. Compiled code and constants are connection-local and disappear when the SQLite connection is closed.

## 2. Procedure definitions

```tcl
proc name {arg1 arg2 ...} {
    commands
    return value
}
```

Example:

```tcl
proc hypotenuse_squared {a b} {
    return [add [mul $a $a] [mul $b $b]]
}
```

Every procedure must return a numeric value on every reachable path. BC-Lite has no string, list, dictionary, object, or general Tcl value type.

### Procedure names

Use ordinary public names for procedures intended to be called by SQLite or by another compilation unit:

```tcl
proc monthly_interest {balance rate} {
    return [div [mul $balance $rate] #12]
}
```

A name beginning with `_` declares a **local procedure**:

```tcl
proc _square {x} {
    return [mul $x $x]
}

proc sum_of_squares {a b} {
    return [add [_square $a] [_square $b]]
}
```

Local procedures are private to their compilation unit. The compiler gives each one an internal unit-qualified name, so two different compilation units may both define `_helper` without colliding. A local procedure may be called only from procedures in its own unit and is not exposed as a user-callable SQLite function.

Do not call or define names beginning with a decimal digit. Digit-prefixed names are reserved for the compiler's internal local-procedure namespace.

### Arguments and locals

Arguments are numeric values and are referenced with `$name`:

```tcl
proc identity {x} {
    return $x
}
```

`set` creates a local variable on first assignment and updates it thereafter:

```tcl
proc square_plus_one {x} {
    set y [mul $x $x]
    set y [add $y #1]
    return $y
}
```

Reading a variable before its first `set` is a compile error. Variables are procedure-local; there are no globals.

## 3. Syntax essentials

There is no support for comments.

Commands are separated by a newline or `;`.

```tcl
set x #1
set y #2; return [add $x $y]
```

A constant number can be in one of two forms:

```tcl
5
#5.0
```

The first form is only available for integers that can fit in a Sqlite3 signed int64. The second string form can be used 
for any integer or exact decimal number.

A variable reference starts with `$`:

```tcl
$x
```

A bracketed command is an expression whose result is supplied to its enclosing command:

```tcl
return [add [mul $x $x] #1]
```

Braces delimit argument lists and command bodies. They are grouping syntax, not string values.

Truth is numeric:

- `#0` is false;
- every non-zero value is true.

## 4. Core special forms

BC-Lite has a deliberately small set of compiler-recognised forms.

### `proc`

Defines a procedure.

```tcl
proc add_tax {price rate} {
    return [mul $price [add #1 $rate]]
}
```

### `set`

Creates or replaces a local variable. `set` is a statement and its result should not be used as an expression.

```tcl
set total [add $subtotal $tax]
```

### `return`

Returns one numeric expression.

```tcl
return [div $sum $count]
```

### `if`

```tcl
if condition {
    then-commands
}
```

or:

```tcl
if condition {
    then-commands
} {
    else-commands
}
```

The else body is optional.

```tcl
proc abs_value {x} {
    if [lt $x #0] {
        return [sub #0 $x]
    }
    return $x
}
```

An empty then-body is useful as an `if not` form:

```tcl
if condition {
} {
    commands-when-false
}
```

### `loop`

Repeats a body indefinitely until `break` or `return` executes.

```tcl
loop {
    body
}
```

### `break`

Exits the nearest enclosing `loop`.

```tcl
loop {
    if [le $x #0] {
        break
    }
    set x [sub $x #1]
}
```

`break` outside a loop is a compile error.

## 5. Common control-flow patterns

BC-Lite intentionally provides only `if`, `loop`, and `break`. The usual higher-level forms are written as patterns.

### While loop

Equivalent to `while condition { body }`:

```tcl
loop {
    if condition {
    } {
        break
    }

    body
}
```

Example:

```tcl
proc factorial {n} {
    set result #1
    loop {
        if [gt $n #1] {
        } {
            break
        }
        set result [mul $result $n]
        set n [sub $n #1]
    }
    return $result
}
```

### Until loop

Equivalent to `until condition { body }`:

```tcl
loop {
    if condition {
        break
    }
    body
}
```

### Do-while loop

Equivalent to `do { body } while condition`:

```tcl
loop {
    body
    if condition {
    } {
        break
    }
}
```

### For loop

Equivalent to `for init condition step { body }`:

```tcl
initialisation
loop {
    if condition {
    } {
        break
    }

    body
    step
}
```

Example:

```tcl
proc sum_to {n} {
    set i #1
    set total 0
    loop {
        if [le $i $n] {
        } {
            break
        }
        set total [add $total $i]
        set i [add $i #1]
    }
    return $total
}
```

### Continue

There is no `continue` special form. Put the remainder of the loop body in the opposite branch, or factor one iteration into a helper procedure.

```tcl
loop {
    if skip-condition {
        set i [add $i #1]
    } {
        normal-body
        set i [add $i #1]
    }
}
```

For a long body, a local helper is clearer:

```tcl
proc _process_one {value} {
    if [eq [mod $value #2] #0] {
        return #0
    }
    return [mul $value $value]
}
```

### Switch-style selection

Use nested `if` forms:

```tcl
proc classify {x} {
    if [eq $x #1] {
        return #10
    } {
        if [eq $x #2] {
            return #20
        } {
            if [eq $x #3] {
                return #30
            } {
                return #0
            }
        }
    }
}
```

### Logical AND

```tcl
if cond1 {
    if cond2 {
        both-true-body
    }
}
```

To produce a numeric Boolean:

```tcl
proc logical_and {a b} {
    if $a {
        if $b {
            return #1
        }
    }
    return #0
}
```

### Logical OR

```tcl
if cond1 {
    body
} {
    if cond2 {
        body
    }
}
```

### Logical NOT

```tcl
proc logical_not {x} {
    if $x {
        return #0
    }
    return #1
}
```

## 6. Arithmetic and precision

All calculations use BCL arbitrary-precision decimal numbers. BC-Lite does not use binary floating point.

The current scale belongs to the connection-local BCL context. Division, square root, transcendental library procedures, and other scale-sensitive operations use that setting.

```tcl
proc one_third {} {
    set_scale #20
    return [div #1 #3]
}
```

Changing scale affects later work using the same connection. A library procedure may temporarily increase scale internally to guard against premature truncation and then restore or reduce the result as required by that procedure's contract.

## 7. Core builtins

Builtins are ordinary calls, so they can be used directly as commands or inside brackets. They cannot be redefined by user source.

### Context and scale

| Function | Meaning |
|---|---|
| `get_scale` | Returns the current non-negative calculation scale. |
| `set_scale scale` | Sets the current scale and returns the resulting scale. |

### Arithmetic

| Function | Meaning |
|---|---|
| `add a b` | `a + b` |
| `sub a b` | `a - b` |
| `mul a b` | `a * b` |
| `div a b` | Decimal division at the current scale; division by zero is an error. |
| `mod a b` | Remainder; a zero divisor is an error. |
| `pow a b` | Raises `a` to the supported numeric exponent `b`. |
| `neg x` | Arithmetic negation. |
| `abs x` | Absolute value. |
| `sqrt x` | Square root at the current scale; a negative input is an error. |

### Comparison

Comparison results are numeric `0` or `1`, except `cmp`.

| Function | Meaning |
|---|---|
| `cmp a b` | Returns a negative value, `0`, or a positive value according as `a < b`, `a == b`, or `a > b`. |
| `eq a b` | Numeric equality. |
| `ne a b` | Numeric inequality. |
| `lt a b` | `a < b` |
| `le a b` | `a <= b` |
| `gt a b` | `a > b` |
| `ge a b` | `a >= b` |

Comparisons are numeric, never lexical: `10` is greater than `2`.

### Number inspection and conversion helpers

| Function | Meaning |
|---|---|
| `scale x` | Returns the decimal scale of `x`. |
| `length x` | Returns the number of significant decimal digits according to BCL semantics. |
| `is_integer x` | Returns `1` when `x` has no fractional part, otherwise `0`. |
| `trunc x scale` | Truncates `x` to the requested number of fractional digits. |

### Random-number helpers

The random builtins use the BCL random facility and return BC-Lite numeric values.

| Function | Meaning |
|---|---|
| `irand limit` | Random integer in the implementation-defined range below `limit`; `limit` must be valid and positive. |
| `frand places` | Random fractional value with the requested decimal precision. |
| `seed value` | Seeds the connection-local random generator and returns the resulting seed/value according to the implementation contract. |

Random procedures are unsuitable for cryptographic use.

### Persistent aggregate/window state

These builtins are meaningful only while the procedure is running as an SQLite aggregate or window-function callback.

| Function | Meaning |
|---|---|
| `state_get index` | Returns the persistent numeric state slot at `index`. |
| `state_set index value` | Replaces the persistent state slot at `index` and returns `value`. |

Indexes are zero-based and must be less than the build-time state-slot limit. State belongs to the current aggregate/window instance, not to the procedure globally.

Typical use:

```tcl
proc running_total_step {value} {
    set total [add [state_get #0] $value]
    return [state_set #0 $total]
}
```

A procedure called outside an aggregate/window state context must not use these functions.

> The exact builtin set can be reduced by compile-time feature switches. A disabled builtin is unavailable and causes an unknown-function compile error.

## 8. Standard libraries

The project can embed and load zero, one, two, or three BC-Lite libraries. Loading level 1 loads `lib.bc-lite`; level 2 loads `lib.bc-lite` and `lib2.bc-lite`; level 3 additionally loads `lib3.bc-lite`.

Library procedures are normal compiled procedures. They may call builtins and procedures from earlier loaded libraries. They are not SQLite functions merely because they are loaded; exposure is controlled separately by the SQLite adapter.

### Library 1: `lib.bc-lite`

This is the traditional bc mathematics library translated to BC-Lite.

| Procedure | Meaning |
|---|---|
| `e x` | Exponential function, `e^x`. |
| `l x` | Natural logarithm. Input must be positive. |
| `s x` | Sine, with `x` in radians. |
| `c x` | Cosine, with `x` in radians. |
| `a x` | Arctangent, result in radians. |
| `j n x` | Bessel function of integer order `n`. |

These functions are scale-sensitive. Set a suitable scale before calling them.

### Library 2: `lib2.bc-lite`

`lib2.bc-lite` adds general mathematical helpers. The BC-Lite port intentionally excludes upstream display/printing procedures because BC-Lite values are numeric and output belongs at the SQLite/JimTcl boundary.

| Procedure | Meaning |
|---|---|
| `p x y` | Raises `x` to the general numeric power `y`. |
| `r x p` | Rounds `x` to `p` fractional places using the library's default rounding rule. |
| `ceil x p` | Ceiling of `x` at precision `p`. |
| `f n` | Factorial. |
| `max a b` | Larger of `a` and `b`. |
| `min a b` | Smaller of `a` and `b`. |
| `perm n k` | Number of permutations of `n` items taken `k`. |
| `comb n r` | Number of combinations of `n` items taken `r`. |
| `fib n` | Fibonacci number. |
| `log x b` | Logarithm of `x` to base `b`. |
| `l2 x` | Base-2 logarithm. |
| `l10 x` | Base-10 logarithm. |
| `root x n` | `n`th root of `x` where supported. |
| `cbrt x` | Cube root. |
| `gcd a b` | Greatest common divisor. |
| `lcm a b` | Least common multiple. |
| `pi s` | Pi calculated to scale `s`. |
| `t x` / `tan x` | Tangent, radians. |
| `a2 y x` / `atan2 y x` | Two-argument arctangent, radians. |
| `sin x` | Alias for `s`. |
| `cos x` | Alias for `c`. |
| `atan x` | Alias for `a`. |
| `r2d x` | Radians to degrees. |
| `d2r x` | Degrees to radians. |
| `frand p` | Random fraction with precision `p`. |
| `ifrand i p` | Random integer/fractional value using integer limit `i` and precision `p`. |
| `i2rand a b` | Random integer in the library-defined interval based on `a` and `b`. |
| `srand x` | Seeds the random generator. |
| `brand` | Random Boolean, `0` or `1`. |
| `ubytes x` | Minimum unsigned byte width needed for integer `x`. |
| `sbytes x` | Minimum signed byte width needed for integer `x`. |
| `s2un x n` | Converts signed `n`-byte integer interpretation to unsigned. |
| `s2u x` | Signed-to-unsigned conversion using the minimum signed width. |
| `band a b` | Bitwise AND on non-negative integer representations. |
| `bor a b` | Bitwise OR. |
| `bxor a b` | Bitwise XOR. |
| `bshl a b` | Logical left shift. |
| `bshr a b` | Logical right shift. |
| `bnotn x n` | Bitwise NOT constrained to `n` bytes. |
| `bnot8/16/32/64 x` | Fixed-width bitwise NOT. |
| `bnot x` | Bitwise NOT using the minimum unsigned width. |
| `brevn x n` | Reverse bits in an `n`-byte value. |
| `brev8/16/32/64 x` | Fixed-width bit reversal. |
| `brev x` | Bit reversal using the minimum unsigned width. |
| `broln x p n` | Rotate left by `p` bits in an `n`-byte value. |
| `brol8/16/32/64 x p` | Fixed-width rotate left. |
| `brol x p` | Rotate left using the minimum unsigned width. |
| `brorn x p n` | Rotate right by `p` bits in an `n`-byte value. |
| `bror8/16/32/64 x p` | Fixed-width rotate right. |
| `bror x p` | Rotate right using the minimum unsigned width. |
| `bmodn x n` | Reduce `x` modulo the range of an `n`-byte unsigned value. |
| `bmod8/16/32/64 x` | Fixed-width unsigned reduction. |

All bit procedures operate on integer-valued inputs. Fractional inputs are invalid or are treated according to the exact library procedure contract; callers should pass integers explicitly.

### Library 3: `lib3.bc-lite`

`lib3.bc-lite` supplies explicit decimal rounding modes modelled on libmpdec/IEEE-style decimal rounding. Each procedure rounds `x` to `places` digits after the decimal point.

| Procedure | Rounding rule |
|---|---|
| `round_down x places` | Toward zero. |
| `round_up x places` | Away from zero when discarded digits are non-zero. |
| `round_floor x places` | Toward negative infinity. |
| `round_ceiling x places` | Toward positive infinity. |
| `round_half_up x places` | Nearest; ties away from zero. |
| `round_half_down x places` | Nearest; ties toward zero. |
| `round_half_even x places` | Nearest; ties to an even final retained digit. |
| `round_half_odd x places` | Nearest; ties to an odd final retained digit. |
| `round_05up x places` | Away from zero when the final retained digit is `#0` or `#5`; otherwise toward zero. |
| `round_trunc x places` | Explicit truncation alias where provided by the library. |

Examples:

```tcl
proc money_even {x} {
    return [round_half_even $x #2]
}

proc tax_ceiling {x} {
    return [round_ceiling $x #2]
}
```

## 9. Writing reliable math procedures

### Validate domains with comparisons

```tcl
proc safe_sqrt {x} {
    if [lt $x #0] {
        return #0
    }
    return [sqrt $x]
}
```

Usually it is better to let a builtin report a domain error than silently return an arbitrary sentinel.

### Use guard digits

For multi-step fractional calculations, temporarily use a scale greater than the requested output precision, then apply the desired final rounding procedure.

```tcl
proc ratio_rounded {a b places} {
    set old_scale [get_scale]
    set_scale [add $places #8]
    set value [div $a $b]
    set result [round_half_even $value $places]
    set_scale $old_scale
    return $result
}
```

### Prefer local helpers

Use `_name` procedures to split a formula without exposing implementation details:

```tcl
proc _cube {x} {
    return [mul [mul $x $x] $x]
}

proc sum_of_cubes {a b} {
    return [add [_cube $a] [_cube $b]]
}
```

### Remember that ordinary command results are discarded

At command level, a normal function call executes but its result is popped:

```tcl
set_scale #20
```

Use brackets when the value is needed by another command:

```tcl
set old [get_scale]
```

## 10. Recursion and forward calls

Recursion and mutual recursion are supported within configured VM limits.

```tcl
proc even {n} {
    if [eq $n #0] {
        return #1
    }
    return [odd [sub $n #1]]
}

proc odd {n} {
    if [eq $n #0] {
        return #0
    }
    return [even [sub $n #1]]
}
```

Because both definitions are in the same compilation unit, the forward call from `even` to `odd` is resolved before the unit commits.

## 11. Limits and errors

The compiler and VM enforce configurable limits including source size, procedure count, arguments, locals, stack depth, recursion depth, bytecode size, constants, symbols, and instructions per invocation.

Typical errors include:

- invalid numeric literal;
- malformed braces or brackets;
- duplicate argument or local name;
- variable read before first assignment;
- unknown or unresolved procedure;
- wrong call arity;
- duplicate public procedure;
- attempt to redefine a builtin;
- `break` outside a loop;
- procedure with no return value;
- division by zero or invalid mathematical domain;
- recursion, stack, bytecode, source, or instruction limit exceeded;
- use of aggregate state outside an aggregate/window invocation.

There is no `catch` form. A runtime error aborts the current BC-Lite invocation and is reported through the embedding C/SQLite API.

## 12. Complete example

```tcl
proc _term {x mean} {
    set d [sub $x $mean]
    return [mul $d $d]
}

proc bounded_sum_of_squares {start finish mean} {
    set x $start
    set total #0

    loop {
        if [le $x $finish] {
        } {
            break
        }

        set total [add $total [_term $x $mean]]
        set x [add $x #1]
    }

    return $total
}
```

This demonstrates a compilation-unit-local helper, locals, nested expressions, a for-style loop, numeric comparison, and an explicit return.
