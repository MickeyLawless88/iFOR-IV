# IV-TRAN — An Interactive ANSI FORTRAN IV Interpreter

*(Runtime banner: `iFOR-IV Rev.0.88`  •  Listing banner: `IV-TRAN Interactive FORTRAN IV Rev. 5.21`)*

IV-TRAN is a single-file, strict **C89** interpreter for a practical, benchmark-driven subset of **ANSI FORTRAN IV**. It runs as an interactive, BASIC-style REPL — type a numbered line to store it, `RUN` to execute, `LIST` to see it again — and it also runs FORTRAN source files in batch mode from the command line. It is written to compile unmodified on both a modern C89/ANSI host (gcc/clang) *and* **Turbo C++ 1.00 on real DOS hardware**, and several of its design choices exist specifically because of quirks discovered while testing on real vintage machines (see [Historical Curiosities & Hardware Workarounds](#historical-curiosities--hardware-workarounds) below).

> **A note on names:** the source carries two different self-identifications. The interactive startup banner (`printBanner()`) prints `iFOR-IV Rev.0.88`, copyright "Lawless Cybernetics Inc. [1966] 2025,2026", while the `LISTING`-mode compile banner (`printSourceListing()`) prints `IV-TRAN Interactive FORTRAN IV Rev. 5.21` under "Retrocomputing Systems Group," with a `Programmer: Mickey W. Lawless` credit line. Both strings live in the code as written; this README uses **IV-TRAN** as the project name throughout, since that's the name in the top-of-file header comment.

---

## Table of Contents

1. [What It Is](#what-it-is)
2. [Feature Summary](#feature-summary)
3. [Language Reference](#language-reference)
4. [FORMAT / Edit Descriptor Reference](#format--edit-descriptor-reference)
5. [Source Form Rules](#source-form-rules)
6. [Building](#building)
7. [Running](#running)
8. [REPL Command Reference](#repl-command-reference)
9. [LISTING and TRACE Output](#listing-and-trace-output)
10. [Internal Architecture](#internal-architecture)
11. [Known Limitations](#known-limitations)
12. [Historical Curiosities & Hardware Workarounds](#historical-curiosities--hardware-workarounds)
13. [Example Session](#example-session)
14. [Error Messages](#error-messages)

---

## What It Is

IV-TRAN parses and executes FORTRAN IV statements directly from text — there is no separate compile step and no generated machine code. Statements are re-parsed with a small recursive-descent expression engine every time they execute. Despite this, it presents itself with the trappings of a real compiler: a startup banner, a `LISTING` mode that prints a paginated source listing and a post-run symbol/label cross-reference exactly like a 1970s mainframe compiler would, and a `TRACE` mode that prints a synthetic "object code" trace of stack-machine-style pseudo-instructions (`LIT`, `LD`, `STO`, `ADD`, `CALL`, `JMP`, …) as each statement runs — an honest accounting of what the interpreter itself is doing, addressed with a synthetic incrementing hex counter, in lieu of real generated object code.

It supports two ways of getting a program in:

- **Interactive, line-numbered entry** — type `10 X = 1` and the line is stored under label `10`; type an unlabeled statement and it executes immediately (classic Microsoft BASIC / DEC BASIC-PLUS style).
- **Batch mode** — pass a filename on the command line and the whole file is loaded and run, using genuine (if relaxed) fixed-form FORTRAN conventions: `C` in column 1 for comments, `*` as a comment marker, and `+ > $ &` as continuation markers.

## Feature Summary

### Advertised in the header comment (and implemented)

- Implicit typing: names starting `I`–`N` are `INTEGER`, everything else is `REAL`, overridable with explicit `INTEGER`/`REAL` declarations.
- `DIMENSION`, up to **3 dimensions** per array.
- Assignment and arithmetic expressions: `+ - * / **`, parentheses.
- Intrinsics: `SIN COS TAN ATAN EXP ALOG ALOG10 SQRT ABS IABS FLOAT IFIX INT MOD AMOD SIGN` (plus several not called out in the header — see below).
- `DO` / `CONTINUE`, including **shared terminal labels** (several nested `DO` loops legally closing on the same trailing `CONTINUE`).
- `GOTO label` (and `GO TO label`, checked separately).
- **Arithmetic IF**: `IF (expr) L1,L2,L3`.
- **Logical IF**: `IF (expr) statement`.
- `FORMAT` with `I, F, E, A, X, H` (Hollerith) edit descriptors, repeat counts, nested groups, and **format reversion** (a `FORMAT` is re-applied from the top, record after record, until the output list is exhausted).
- `PRINT` / `WRITE` with implied-`DO` output lists, including nested implied-`DO`s.
- `READ`, list-directed, from `stdin`.
- Hollerith constants: `nHtext` (in `FORMAT`s, the full `n`-character text is emitted) and `nHc` (as an expression primary — see the quirk noted under [Historical Curiosities](#historical-curiosities--hardware-workarounds)).
- `STOP` / `END`.
- The interactive line editor described above, plus `RUN`/`LIST`/`NEW`/`LOAD`/`SAVE`/`BYE`.

### Implemented but *not* called out in the header comment

The top-of-file comment block is a bit stale relative to the actual code. The following are real, working features that the header doesn't mention:

- **`SUBROUTINE` / `CALL` / `RETURN`**, with genuine call-by-reference semantics (see [SUBROUTINEs](#subroutines-and-call-by-reference) below). The header's "NOT implemented" list still says *"subprograms (FUNCTION/SUBROUTINE/CALL)"* — that's no longer accurate for `SUBROUTINE`/`CALL`. Multi-statement `FUNCTION` subprograms genuinely are **not** implemented, but one-line statement functions are (next bullet).
- **One-line statement functions**, e.g. `IFUNCT(Y) = Y**2` — FORTRAN's classic single-expression function shorthand, up to 20 of them, up to 3 arguments each.
- **`CHARACTER` data**, including `CHARACTER*n` declared length, `CHARACTER` arrays, string **concatenation** with `//`, and ANSI-style blank-padded string comparison.
- **`PARAMETER (name = expr, ...)`** named constants (not enforced as immutable, but functionally equivalent to a one-time assignment respecting the name's declared type).
- **`DATA name/val/...`** initialization, including array fill-by-repetition (values recycle across a longer array, matching classic `DATA` semantics for under-supplied lists).
- Extra intrinsics beyond the header's list: `MAX0 MAX1 AMAX0 AMAX1 MIN0 MIN1 AMIN0 AMIN1 MAX MIN`.
- Extra `FORMAT` descriptors beyond `I F E A X H`: **`T`** (absolute column positioning) and **`/`** (explicit record/slash separator).
- **Carriage-control interpretation** on the first character of every formatted output record: `' '` normal, `'0'` double-space, `'1'` new page (form feed), `'+'` overprint (carriage return, no line advance).
- **Logical/relational operators**: `.LT. .LE. .EQ. .NE. .GT. .GE. .NOT. .AND. .OR.` (used by both arithmetic and logical `IF`).
- **List-directed I/O**: `PRINT *, ...` / `WRITE(6,*) ...`, and an inline quoted `FORMAT` string instead of a label — `PRINT '(F10.4)', X`.
- **`DIMENSION` lower:upper bounds** (`DIMENSION A(0:10)`), a FORTRAN 77-ism layered on top of the otherwise-FORTRAN-IV feature set.
- **`LISTING`** and **`TRACE`** modes (compiler-style source listing + cross-reference; synthetic pseudo-object trace).
- Whole-array references in I/O lists (`PRINT *, ARR` prints every element without an explicit implied-`DO`).

## Language Reference

### Data types

| Type | Storage | Notes |
|---|---|---|
| `INTEGER` | `long` | Implicit for names starting `I`–`N` |
| `REAL` | `float` | Implicit for all other starting letters |
| `CHARACTER*n` | fixed-length byte buffer | Declared length defaults to 1; truncated/blank-padded on assignment |

Every value flowing through the expression engine is a tagged union (`VAL`: `isReal`, `isStr`, integer, float, and a 128-byte string buffer), so integers, reals, and strings freely intermix in expressions, with the usual FORTRAN promotion rule: **any operand that's real makes the whole binary operation real.**

### Declarations

```fortran
INTEGER I, J, K
REAL    X, Y
CHARACTER*20 NAME
DIMENSION A(10), B(5,5), C(2,2,2)
CHARACTER*4 GRID(10,10)
PARAMETER (PI = 3.14159, MAXN = 100)
DATA A/1,2,3,4,5,6,7,8,9,10/
DATA STAR/'*'/ DASH/'-'/
```

- Arrays support 1–3 dimensions.
- `*len` and `(dims)` may appear in either order on a `CHARACTER` declaration line.
- Declaring an already-allocated name (e.g. re-declaring dimensions on a `SUBROUTINE`'s formal parameter, which is aliased to the caller's real array) updates bookkeeping only — it never reallocates, so a by-reference parameter's data survives.

### Expressions and operator precedence

From loosest to tightest binding (the recursive-descent parser's actual call chain):

1. `.OR.`
2. `.AND.`
3. `.NOT.` (unary)
4. `.LT. .LE. .EQ. .NE. .GT. .GE.` (relational, single level, left-to-right)
5. `//` (string concatenation)
6. `+ -` (binary, left-to-right)
7. `* /` (left-to-right)
8. `**` (power, **right-associative**)
9. unary `+ -`
10. primary: literals, parenthesized sub-expressions, variable/array references, function/intrinsic calls, statement-function calls

Integer `**` with a non-negative integer exponent is computed by repeated integer multiplication (never via `pow()`), preserving integer type; any other combination promotes to real and uses `pow()`.

### Intrinsic functions

```
SIN COS TAN ATAN EXP ALOG ALOG10 SQRT ABS IABS
FLOAT IFIX INT MOD AMOD SIGN
MAX0 MAX1 AMAX0 AMAX1 MIN0 MIN1 AMIN0 AMIN1 MAX MIN
```

`MAX`/`MIN` family functions accept any number of arguments; the `0`-suffixed forms (`MAX0`, `MIN0`) return `INTEGER`, the rest return `REAL`.

### Statement functions

```fortran
IFUNCT(Y) = Y**2 + 1
```

A one-line function is recognized when an assignment target is followed by `(`, the parenthesized list contains only bare argument names (no subscript expressions), and a `=` follows the closing paren. Up to 20 statement functions, up to 3 arguments each. Calling one temporarily overwrites the argument variables' *global* values for the duration of the call and restores them afterward — there is no separate local scope.

### SUBROUTINEs and call-by-reference

```fortran
      SUBROUTINE SWAP(A, B)
      T = A
      A = B
      B = T
      RETURN
      END

      CALL SWAP(X, Y)
```

- `SUBROUTINE` bodies are located by a pre-scan pass (`prescanSubprograms`) before execution begins, so a subroutine may be defined anywhere in the file — before or after its call sites — and normal top-to-bottom "falling into" a subroutine body during ordinary execution is skipped over entirely.
- **A bare variable name (scalar or whole array, no subscript) passed as an actual argument is a true alias**: for the duration of the call, the formal parameter name and the caller's storage are, literally, the same `VAR*`. Writes inside the callee are visible to the caller the instant the call returns — genuine FORTRAN pass-by-reference, implemented via pointer aliasing rather than address arithmetic.
- Anything else passed as an actual argument — a literal, an expression, or a single subscripted array element — is evaluated **once** into a scratch variable and bound read-only. Writes to such a parameter inside the callee will **not** propagate back to the caller, since the `VAR` model has no way to alias a single array element. This is a documented, deliberate simplification.
- `END` inside a subroutine body acts as an implicit `RETURN`.
- Call depth is capped at 10; each subroutine accepts up to 8 arguments.

### Control flow

```fortran
      DO 10 I = 1, 100, 2
      X = X + I
   10 CONTINUE

      IF (X .GT. 0.0) GOTO 20
      IF (X .EQ. 0) X = 1, Y = 2, GOTO 30      ! (illustrative only — see note)
   20 CONTINUE
      IF (X) 10, 20, 30                          ! arithmetic IF: neg/zero/pos targets
```

- `DO var = start, stop [, step]` — `step` defaults to 1 (and any zero step is silently coerced to 1). The loop variable's declared type (integer or real) governs whether integer or floating comparisons/increments are used, and real-valued loop bounds are compared with a small epsilon (`1e-4`) to absorb float rounding.
- Multiple `DO` loops may legally share one terminal label; closing that label unwinds however many loops are waiting on it, from innermost outward.
- **Logical `IF (expr) statement`** executes the trailing statement (which may itself be almost anything, including another `IF`) only if the condition is true.
- **Arithmetic `IF (expr) L1,L2,L3`** branches to `L1` if the expression is negative, `L2` if zero, `L3` if positive.
- `GOTO label` / `GO TO label` unconditional branch.

### Input / Output

```fortran
      WRITE(6,100) X, Y
      PRINT 100, X, Y
      PRINT *, 'RESULT =', X
      PRINT '(F10.4)', X
      READ(5,*) A, B, C
      READ *, N
  100 FORMAT(1X, 2F10.4)
```

- `WRITE(unit,label)` / `PRINT label,` reference a separately declared `FORMAT` statement by label.
- `WRITE(unit,*)` / `PRINT *,` is list-directed: values are printed with fixed generic widths (strings as-is, reals as `%14.6f`, integers as `%10ld`), one line per call, no `FORMAT` involved.
- An inline quoted format string may be given directly in place of a label: `PRINT '(...)'`.
- Implied-`DO` output lists are supported and may nest: `PRINT 100, (A(I), I=1,10)` or `PRINT 100, ((M(I,J),J=1,3),I=1,3)`.
- A bare array name in an output list (no subscript) expands to every element of the array in storage order.
- `READ` is list-directed only, reads one line of input per call, and parses each numeric value with a small hand-rolled scanner rather than `scanf` (see [Historical Curiosities](#historical-curiosities--hardware-workarounds)). Subscripted array elements are supported as read targets.

### Hollerith constants

- In a `FORMAT` string, `nH` followed by exactly `n` characters emits that literal text verbatim into the output record — the classic Hollerith edit descriptor, fully supported.
- As an **expression primary** (e.g. inside a `DATA` statement), `nHtext` is scanned past all `n` characters, but only the **first** character's ASCII code becomes the resulting integer value — a documented simplification suited to the common single-character case (`DATA STAR/1H*/`) rather than true packed multi-character Hollerith-as-integer storage.

## FORMAT / Edit Descriptor Reference

| Descriptor | Meaning |
|---|---|
| `Iw` | Integer, field width `w` (default 6 if omitted/zero) |
| `Fw.d` | Real, fixed-point, width `w` (default 10) / decimals `d` (default 2) |
| `Ew.d` | Real, exponential (`%e`-style), width `w` (default 12) / decimals `d` (default 4) |
| `Aw` | Character/string, width `w` (0 = natural length) |
| `Xw`* | Skip `w` blank columns (repeat count before `X` gives the width, e.g. `5X`) |
| `nH...` | Hollerith literal text, `n` characters, emitted verbatim |
| `'text'` | Quoted literal text, emitted verbatim |
| `Tn` | Move to absolute column `n` (pads with blanks if not already past it) |
| `/` | Force a new output record (like a line break mid-format) |
| `(...)` | Grouping, may carry its own repeat count prefix, e.g. `3(I4,F6.2)` |
| repeat count | Any descriptor or group may be prefixed with an integer repeat count |

- Leading carriage-control character: consumed from column 1 of the completed record and interpreted as `' '` (advance one line), `'0'` (blank line then advance), `'1'` (form feed / new page), or `'+'` (overprint, no advance) — anything else is treated as a normal advance.
- **Format reversion**: if a `FORMAT` is applied to more output values than one pass through it consumes, the whole format is reapplied from the beginning, producing additional records, until the value list is exhausted (or no further descriptor makes progress, which stops the loop defensively).
- If there are **zero** output values (e.g. `PRINT 100` with no list), the format's literal text is still emitted exactly once.

## Source Form Rules

IV-TRAN reads **free-format** lines rather than true fixed-column (1-5 label / 6 continuation / 7-72 statement) FORTRAN source. Rules, in both interactive and batch/file input:

- A line beginning with digits is a **labelled** program line; the digits (up to the first non-digit) become the label, and the rest of the line (whitespace-trimmed) is the statement text.
- A line with no leading digits is either stored as an **unlabelled** program line (batch file mode) or **executed immediately** (interactive mode).
- **Comments** (batch/file mode only): a line with `C` or `c` in column 1, or a line whose first non-blank character is `*`.
- **Continuation** (batch/file mode only): a line whose first non-blank character is `+`, `>`, `$`, or `&` is appended (space-joined) to the previous line, regardless of that line's label.
- Keywords and variable names should be typed in **UPPERCASE**, as in classic FORTRAN source (the tokenizer folds case for keyword/name matching, but this is the intended convention).
- Variable/subroutine/statement-function names are significant to **7 characters** (an 8th byte is reserved for the terminating NUL) — an 8-character-or-longer name in source is silently truncated when stored and looked up.

## Building

### Modern hosts (gcc / clang)

```sh
cc -std=c89 -pedantic -O2 -lm -o ivtran ivtran.c
```

`-lm` is required for the math intrinsics (`sin`, `cos`, `pow`, `log`, etc.).

### Turbo C++ 1.00 / DOS

```
TCC -mc IVTRAN.C
```

(or the memory model of your choice — the source has no model-specific code). The `FARBIG` macro automatically expands to `far` under `__TURBOC__`/`__BORLANDC__`, moving the largest static tables (`prog[]`, `fmts[]`, `outbuf[]`, and the subroutine scratch-argument buffers) out of the default 64KB data segment (`DGROUP`) so they don't count against that segment's shared budget alongside every other global. On any other host this macro is a no-op.

## Running

### Interactive mode

```sh
./ivtran
```

Drops straight into a `READY` / `Ok` prompt. Type numbered lines to build a program, unlabelled lines to execute immediately, and use the [REPL commands](#repl-command-reference) below to manage it.

### Batch mode

```sh
./ivtran myprogram.f
./ivtran -l myprogram.f          # with compiler-style LISTING + cross-reference
./ivtran -t myprogram.f          # with the pseudo-object TRACE
./ivtran -l -t myprogram.f       # both
```

Command-line flags (order-independent, any non-flag argument is taken as the filename):

| Flag | Effect |
|---|---|
| `-l`, `-list`, `-listing` | Turn on `LISTING` mode before running |
| `-t`, `-trace` | Turn on `TRACE` mode before running |

## REPL Command Reference

| Command | Effect |
|---|---|
| `RUN` | Run the currently stored program from the top |
| `LIST` | Print the stored program, in label order |
| `NEW` | Clear the stored program, variables, and `DO`/format tables |
| `EDIT` / `ED` / `AUTO` | Enter the [line editor submenu](#edit-submenu) |
| `LOAD "file"` | Clear the program and load a new one from `file` |
| `SAVE "file"` | Write the current program out to `file` |
| `LISTING` | Report current `LISTING` on/off status (no change) |
| `LISTING ON` / `LISTING OFF` | Enable/disable compiler-style listing + cross-reference on `RUN` |
| `TRACE` | Report current `TRACE` on/off status (no change) |
| `TRACE ON` / `TRACE OFF` | Enable/disable the pseudo-object execution trace |
| `BYE` / `QUIT` | Exit ("LOGGED OFF") |
| *(numbered line)* | Store that line under its label |
| *(unlabelled statement)* | Execute immediately |

Any line typed but not a recognized command is handed to `loadLine`: if it starts with a label, it's stored silently (no `Ok` echoed — matching the behavior of classic line-numbered BASIC systems where storing a line produces no prompt); if unlabelled, it's executed immediately and `Ok` is printed afterward.

### Edit submenu

Entered via `EDIT` / `ED` / `AUTO`. Unlike some line editors, **lines are not auto-numbered** — a label is stored only if you type one, exactly matching how you'd reference it elsewhere (a `FORMAT` needs a label to ever be useful; a bare `CONTINUE` needs one only if it's a `DO`/`GOTO` target). Leave the submenu with:

- `RUN` or `EX` — executes the assembled listing and returns to the main prompt.
- **Ctrl-Z** (ASCII 26) as the first byte of input, or plain **EOF** — aborts back to the main prompt without running anything.

A `FORMAT` statement typed with no label prints a warning (`?Format statement has no label -- it can never be referenced`) but is still stored.

## LISTING and TRACE Output

**`LISTING ON`** produces, on `RUN`:

1. A pre-run banner and full sequential source dump (`IV-TRAN Interactive FORTRAN IV Rev. 5.21`, compile date/time, program name from a `PROGRAM` statement or `MAIN` if none, programmer credit, one numbered line per statement).
2. After execution, a **Symbol Cross-Reference Listing** (every variable, array, and statement function, with a per-source-line reference-type code: `s`=Specified/declared, `/`=`DATA`, `d`=DO index, `=`=assignment target, `u`=used, `i`=input, `o`=output) and a **Label Cross-Reference Listing** (every statement label, with codes `s`=defining line, `d`=DO target, `@`=FORMAT statement, `f`=FORMAT usage in WRITE/PRINT/READ, `g`=GOTO target, `i`=arithmetic-IF target).

These cross-references are built by a **static scan of the source text**, not a runtime trace — a variable read inside a loop body gets exactly one entry per source line it appears on, matching how a real compiler's cross-reference works, not once per iteration.

**`TRACE ON`** prints, live during execution, a synthetic object-code-style trace: each executed statement's label and source text, followed by the sequence of pseudo-instructions the interpreter performed to carry it out (`LIT`/`LITS`/`LITH` for literals, `LD`/`LDX`/`STO`/`STOX` for loads/stores, `ADD`/`SUB`/`MUL`/`DIV`/`PWR`/`CAT` for operators, `CMP` for comparisons, `CALL`/`RET` for subroutine linkage, `FOR`/`LOOP`/`ENDFOR` for `DO` loops, `JMP`/`AIF`/`IF` for branches, `WRITE`/`PRNT`/`READ` for I/O, `HALT` for `STOP`/`END`), each tagged with a synthetic incrementing hex address.

## Internal Architecture

### Core data model

- **`VAL`** — a tagged-union runtime value: `isReal`, `isStr`, a `long`, a `float`, and a 128-byte string buffer. Every expression evaluation produces one of these; helper functions `VI()`/`VF()` coerce to integer/float on demand (a string's "numeric value" is its first character's code).
- **`VAR`** — a named storage slot: type (0=integer, 1=real, 2=character), scalar or array (up to 3 dims, with per-dimension lower bound and size), and the appropriate backing storage (`long*`/`float*`/`char*` arrays, or scalar fields). Up to 150 live variables (`MAXVARS`).

### Program storage

- **`prog[]`** (`MAXLINES` = 250) — one entry per stored line: label, raw statement text, in-use flag. Interactive `storeLine()` keeps this array sorted by label and overwrites in place if the same label is retyped; batch `appendLineDirect()` preserves literal file order (needed because many lines legitimately share label `0`).
- **`fmts[]`** (`MAXFORMATS` = 100) — a separate pre-scanned table mapping `FORMAT` labels to their text, built by `prescanFormats()` before every run (and before saving a `LISTING`).
- **`subs[]`** (`MAXSUBS` = 20) — a separate pre-scanned table of every `SUBROUTINE`'s name, formal parameters, and body/end line indices, built by `prescanSubprograms()`. This lets `CALL` jump to a subroutine regardless of source order and lets top-to-bottom execution skip cleanly over subroutine bodies it encounters along the way.

### Runtime stacks

- **`dostk[]`** (`MAXDO` = 20) — one frame per active `DO` loop: loop variable, stop/step (kept in both integer and float form), the resumption index, and the terminal label. Popped (possibly several at once) whenever execution reaches a line whose label matches the top frame's terminal label.
- **`callstack[]`** (`MAXCALLDEPTH` = 10) — one frame per active `CALL`: the index to resume at on `RETURN`, and up to 8 formal-parameter-name → actual-`VAR*` bindings. `getvar()`/`findvar()`-style lookups consult the top call frame's bindings first, which is what makes aliasing work transparently throughout the rest of the interpreter.

### Expression engine

A conventional recursive-descent parser operating directly on a `char**` cursor into the statement text (no separate tokenizer/token array) — see [precedence table](#expressions-and-operator-precedence) above. The same engine is reused for assignment right-hand sides, `IF` conditions, `DO` bounds, array subscripts, output-list items, and implied-`DO` control clauses.

### FORMAT engine

`applyFormatItems()` walks the format string recursively (parenthesized groups recurse), consuming values from a flat `outbuf[]` (`MAXVALS` = 350) that was pre-populated by evaluating the entire output list (including expanding any implied-`DO`s and whole-array references) before formatting begins. `applyFormat()` wraps this with the format-reversion loop described above, and `emitRecord()` handles the carriage-control byte on the way out.

### Execution loop

`runFrom()` walks `prog[]` by index (`pc`), skipping unused slots and — importantly — skipping straight over any line range identified as a `SUBROUTINE` body (entered only via `CALL`, never by falling into it). `execIndex()` runs one line's statement, then checks whether the line's label closes one or more pending `DO` frames, looping back into the body if so.

## Known Limitations

Explicitly **not implemented** (per the top-of-file header comment, still accurate for these items):

- `COMMON` / `EQUIVALENCE`
- Multi-statement `FUNCTION` subprograms (one-line statement functions and `SUBROUTINE`/`CALL` **are** implemented, despite the header's stale wording — see above)
- Computed `GOTO`
- `COMPLEX` / `DOUBLE PRECISION`
- True fixed-column (1-5 label / 6 continuation / 7-72 statement) source form — free-format reading is used instead

Practical/structural limits baked into fixed-size tables:

| Limit | Value | Constant |
|---|---|---|
| Stored program lines | 250 | `MAXLINES` |
| Live variables | 150 | `MAXVARS` |
| Statement/line length | 240 chars | `LINELEN` |
| Nested `DO` loops | 20 | `MAXDO` |
| Flattened output-list values per I/O statement | 350 | `MAXVALS` |
| `FORMAT` statements | 100 | `MAXFORMATS` |
| One-line statement functions | 20, 3 args each | `MAXSTMTFUNC` / `MAXSFARGS` |
| `SUBROUTINE`s | 20, 8 args each | `MAXSUBS` / `MAXSUBARGS` |
| `CALL` nesting depth | 10 | `MAXCALLDEPTH` |
| String value length | 127 chars + NUL | (`VAL.s[128]`) |
| Symbol name significance | 7 characters | (`VAR.name[8]`, `readName()`) |

Other behavioral notes:

- `READ` is list-directed only and only ever consumes numeric values from a single line of input per call; it cannot read `CHARACTER` data.
- Array actual arguments to `CALL` alias the *whole array*; a single subscripted element passed as an actual argument is a read-only snapshot, not a true reference.
- `MOD`/integer division by zero is a fatal runtime error, not a trap/signal.

## Historical Curiosities & Hardware Workarounds

A number of implementation choices in the source exist specifically to work around problems observed on **real vintage hardware** during testing, and are called out in comments in the code itself:

- **`READ` never calls `scanf`.** Numeric input is parsed by a hand-written `parseSignedNumber()` that accumulates digits with integer arithmetic and floating-point **multiplication only** — deliberately never division — because a `scanf`-based numeric parser was found to hang indefinitely on at least one piece of real hardware this project was tested against, even after the same input worked fine with a plain `fgets()` + manual parse.
- **File and stdin I/O are explicitly unbuffered** (`setvbuf(..., _IONBF, 0)`), both for `LOAD`ed source files and for interactive `stdin`, because a buffered `fgets()` was observed to hang on a real machine reading a file that DOS's own unbuffered `TYPE` command read instantly — suggesting the hardware/BIOS's buffering layer, not the file or disk itself, was at fault.
- **CPU-time reporting after a `RUN` uses integer arithmetic only** (whole seconds and centiseconds computed from `clock()` ticks via `/` and `%` on `long`s, never on a `float`) — noted in the source as the *only* other floating-point-adjacent operation a purely-integer FORTRAN program would otherwise trig, making it a deliberate target for elimination on hardware without an FPU/coprocessor.
- **The 64KB Turbo C `DGROUP` problem is handled explicitly.** Borland/Turbo C's default data segment caps *all* combined static/global data at 64KB — a stricter, more easily-missed limit than "no single array over 64KB." The largest tables (`prog[]`, `fmts[]`, `outbuf[]`, and the subroutine scratch-argument buffers) are marked `far` via the `FARBIG` macro specifically to keep the *sum* of ordinary globals under that ceiling; the macro is a no-op on every other host.
- **`loadFile()` caps itself at 20,000 read attempts** as a defensive-only bound (not a real file-size limit), because some environments — DOSBox local-directory mounts among them — were observed not to reliably signal end-of-file, which would otherwise spin the load loop forever with no visible symptom.
- **Hollerith constants in expression context keep only their first character's code**, even though the parser correctly advances past all `n` characters of `nHtext` — a deliberate, documented simplification rather than a bug, aimed at the common single-character Hollerith idiom (`DATA STAR/1H*/`) rather than true packed-character-as-integer storage.
- **The header comment is out of date relative to the code.** It still lists `SUBROUTINE`/`CALL` under "NOT implemented," but a full call-by-reference subroutine mechanism (pre-scanning, a real call stack, formal/actual parameter aliasing) exists and works. Worth knowing if you're reading the top-of-file comment as a spec.

## Example Session

```
iFOR-IV Rev.0.88
Interactive ANSI FORTRAN IV Compiler/Interpreter
Copyright [1966] 2025,2026 (C) Lawless Cybernetics Inc.
41312 BYTES WORKSPACE

READY

Ok
10 DIMENSION A(5)
20 DO 30 I = 1, 5
30 A(I) = I * I
40 PRINT *, 'SQUARES:'
50 PRINT 100, (A(I), I=1,5)
60 FORMAT(1X, 5I6)
70 STOP
RUN

 SQUARES:
     1     4     9    16    25

EXECUTION TERMINATED.  CPU TIME: 0.00 SEC
Ok
BYE

LOGGED OFF
```

A `SUBROUTINE` example:

```fortran
      SUBROUTINE AREA(R, A)
      A = 3.14159 * R * R
      RETURN
      END

10    R = 2.0
20    CALL AREA(R, RESULT)
30    PRINT *, 'AREA =', RESULT
40    STOP
```

## Error Messages

Runtime errors are reported by `fatal()` in a compact, classic-interpreter style: `?<message> in <label>` (or just `?<message>` if the current line has no label), with the message's first letter capitalized and the rest lowercased. Example messages produced by the code: `?Syntax error`, `?Division by zero`, `?Subscript out of range`, `?Undefined line number`, `?Undefined function`, `?Undefined subroutine`, `?Return without call`, `?Array not dimensioned`, `?Out of memory`, `?Illegal function call`. A `RUN` that hits a fatal error still prints a completion line, but reports **`EXECUTION TERMINATED ABNORMALLY`** instead of a clean termination, and (with `LISTING ON`) still produces the post-run cross-reference.
