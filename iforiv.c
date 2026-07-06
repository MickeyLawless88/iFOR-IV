/* ------------------------------------------------------------------
      iFOR IV  --  An interactive ANSI FORTRAN IV interpreter
   Written in strict C89 for portability to Turbo C / DOS as well
   as modern hosts. Written by Mickey White Lawless of 
   Lawless Cybernetics Incorporated, July 05. 2026  0010:26PM
   ------------------------------------------------------------------

   Supported (practical, benchmark-driven) subset of FORTRAN IV:
     - Implicit typing (I-N integer, else real) + INTEGER/REAL decls
     - DIMENSION (up to 3 dims per array)
     - Assignment, arithmetic expressions (+ - * / **), parens
     - Intrinsics: SIN COS TAN ATAN EXP ALOG ALOG10 SQRT ABS IABS
                   FLOAT IFIX INT MOD AMOD SIGN
     - DO / CONTINUE (shared terminal labels supported)
     - GOTO label
     - Arithmetic IF (expr) L1,L2,L3
     - Logical IF (expr) statement
     - FORMAT with I,F,E,A,X,H (Hollerith) edit descriptors,
       repeat counts, nested groups, format reversion
     - PRINT / WRITE with implied-DO output lists, incl. nested
     - READ (list directed, from stdin)
     - Hollerith constants  nHtext  and  nHc  (single char)
     - STOP / END
     - Interactive line editor (BASIC-style: type "10 ..." to store
       a line, RUN/LIST/NEW/LOAD/SAVE/BYE to control it), plus
       immediate-mode execution of unlabelled statements, plus
       batch mode via a filename on the command line.

   NOT implemented: COMMON/EQUIVALENCE, subprograms (FUNCTION/
   SUBROUTINE/CALL), computed GOTO, complex/double precision,
   true fixed-column (1-5/6/7-72) source form -- this reads free-
   format lines instead. Keywords and variable names should be
   typed in UPPERCASE, as in classic FORTRAN source.
   ------------------------------------------------------------------ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

/* Turbo C/Borland C, like most 16-bit DOS compilers, caps the total
   of ALL ordinary static/global data at 64KB (the default data
   segment, DGROUP) -- a separate, stricter limit than the "one array
   over 64KB" limit, and one that a handful of individually-modest
   tables can still add up to exceed. FARBIG marks the largest tables
   as explicitly 'far', moving them out of DGROUP into their own
   segment(s) so they don't count against that shared budget. This is
   a no-op everywhere else, including standard C89 hosts, since it
   only expands to anything under the Borland-specific macros below. */
#if defined(__TURBOC__) || defined(__BORLANDC__)
#define FARBIG far
#else
#define FARBIG
#endif

#define MAXLINES   250
#define MAXVARS    150
#define LINELEN    240
#define MAXDO      20
#define MAXVALS    350
#define MAXFORMATS 100

/* ---------------- value type ---------------- */
typedef struct {
    int  isReal;   /* 0 = integer (long), 1 = real (float) */
    int  isStr;    /* 1 = this VAL is a string (literal or CHARACTER var) */
    long i;
    float r;
    char s[128];
} VAL;

static VAL mkint(long v)  { VAL x; x.isReal=0; x.isStr=0; x.i=v; x.r=(float)v; x.s[0]=0; return x; }
static VAL mkreal(float v){ VAL x; x.isReal=1; x.isStr=0; x.r=v; x.i=(long)v; x.s[0]=0; return x; }
static VAL mkstr(const char *t) {
    VAL x; x.isReal=0; x.isStr=1; x.i=0; x.r=0.0f;
    strncpy(x.s, t, sizeof(x.s)-1); x.s[sizeof(x.s)-1]=0;
    return x;
}
static long   VI(VAL v){ if (v.isStr) return (unsigned char)v.s[0]; return v.isReal ? (long)v.r : v.i; }
static float  VF(VAL v){ if (v.isStr) return (float)(unsigned char)v.s[0]; return v.isReal ? v.r : (float)v.i; }

/* ---------------- variables ---------------- */
typedef struct {
    char name[8];
    int  type;      /* 0=integer 1=real 2=character */
    int  isArray;
    int  ndims;
    int  dims[3];   /* SIZE of each dimension (upper-lower+1), not the upper bound itself */
    int  lo[3];     /* lower bound of each dimension; 1 unless an explicit lower:upper was given */
    long size;
    long  *iarr;
    float *rarr;
    long  ival;
    float rval;
    char  *carr;    /* CHARACTER array: size*clen bytes, no NUL separators */
    char  *cval;    /* CHARACTER scalar: clen bytes + 1 NUL */
    int   clen;      /* declared length for CHARACTER*len */
} VAR;

static VAR vars[MAXVARS];
static int nvars = 0;

/* ---- one-line statement functions, e.g. IFUNCT(Y) = Y**2 ---- */
#define MAXSTMTFUNC 20
#define MAXSFARGS   3
typedef struct {
    char name[8];
    char args[MAXSFARGS][8];
    int  nargs;
    char body[LINELEN];
} STMTFUNC;
static STMTFUNC stmtfuncs[MAXSTMTFUNC];
static int nstmtfuncs = 0;

/* ---- SUBROUTINE / CALL / RETURN ----
   Formal parameters get real call-by-reference semantics: rather
   than copying values in and out, a call frame binds each formal
   name directly to the actual argument's VAR* for the duration of
   the call, checked by getvar() before the normal global lookup.
   That means a formal parameter and the caller's actual argument
   are, for the length of the call, literally the same storage --
   exactly like real FORTRAN's pass-by-address, achieved here by
   aliasing pointers instead of addresses. Only bare variable names
   (scalar or whole array, no subscript) get true aliasing; any
   other actual argument (a literal, an expression, a subscripted
   element) is evaluated once and bound to a scratch VAR instead, so
   it works as a read-only value but writes to it inside the callee
   won't propagate back -- a documented simplification, since our
   VAR model has no notion of aliasing a single array element. */
#define MAXSUBS      20
#define MAXSUBARGS   8
#define MAXCALLDEPTH 10

typedef struct {
    char name[8];
    char args[MAXSUBARGS][8];
    int  nargs;
    int  defIdx;   /* index of the SUBROUTINE statement itself */
    int  bodyIdx;  /* index of the first statement inside it */
    int  endIdx;   /* index of its END statement */
} SUBPROG;
static SUBPROG subs[MAXSUBS];
static int nsubs = 0;

typedef struct {
    char formalName[8];
    VAR *actual;
} PARAMBIND;
typedef struct {
    int returnIdx;   /* index of the CALL statement; resumed at returnIdx+1 */
    int nbinds;
    PARAMBIND binds[MAXSUBARGS];
} CALLFRAME;
static CALLFRAME callstack[MAXCALLDEPTH];
static int ncallstack = 0;
static VAR scratchvars[MAXCALLDEPTH][MAXSUBARGS]; /* homes for non-aliasable actual args */
static char FARBIG scratchbuf[MAXCALLDEPTH][MAXSUBARGS][128]; /* backing storage for scratch string args */

static SUBPROG *findSubprog(const char *nm)
{
    int i;
    for (i=0;i<nsubs;i++) if (!strcmp(subs[i].name, nm)) return &subs[i];
    return NULL;
}

static SUBPROG *findSubprogAtLine(int lineIdx)
{
    int i;
    for (i=0;i<nsubs;i++) if (subs[i].defIdx == lineIdx) return &subs[i];
    return NULL;
}

static STMTFUNC *findStmtFunc(const char *nm)
{
    int i;
    for (i=0;i<nstmtfuncs;i++) if (strcmp(stmtfuncs[i].name, nm)==0) return &stmtfuncs[i];
    return NULL;
}

/* ---------------- program storage ---------------- */
typedef struct {
    long label;
    char text[LINELEN];
    int  used;
} PLINE;

static PLINE FARBIG prog[MAXLINES];
static int nlines = 0;

typedef struct {
    long label;
    char text[LINELEN];
} FMTENT;
static FMTENT FARBIG fmts[MAXFORMATS];
static int nfmts = 0;

/* ---------------- DO stack ---------------- */
typedef struct {
    char name[8];
    int  isReal;
    float stop, step;
    long  istop, istep;
    int   bodyIdx;   /* program index right after the DO stmt */
    long  endLabel;
} DOFRAME;
static DOFRAME dostk[MAXDO];
static int ndostk = 0;

static int pc = 0;            /* current program index */
static int running = 0;
static int hadError = 0;
static int stopFlag = 0;

/* forward decls */
static VAL parseExprTop(char **p);
static VAL parseConcatExpr(char **p);
static void execIndex(int idx);
static void runAndReport(int startIdx);
static int findLabel(long lab);
static void runFrom(int startIdx);
static VAL evalLogical(char **p);

/* ------------------------------------------------------------------
   small utilities
   ------------------------------------------------------------------ */
static void fatal(const char *msg)
{
    char cap[80]; int i;
    strncpy(cap, msg, sizeof(cap)-1); cap[sizeof(cap)-1]=0;
    if (cap[0]) cap[0] = (char)toupper((unsigned char)cap[0]);
    for (i=1; cap[i]; i++) cap[i] = (char)tolower((unsigned char)cap[i]);
    if (pc >= 0 && pc < nlines && prog[pc].used && prog[pc].label)
        fprintf(stderr, "\n?%s in %ld\n", cap, prog[pc].label);
    else
        fprintf(stderr, "\n?%s\n", cap);
    if (running) { running = 0; }
    hadError = 1;
}

static void skipsp(char **p)
{
    while (**p == ' ' || **p == '\t') (*p)++;
}

/* Scans a quoted string starting at the opening apostrophe in *s.
   A doubled apostrophe '' inside the string is the standard FORTRAN
   escape for a literal apostrophe. If buf is non-NULL, the
   unescaped text is copied into it (bufsize-1 chars max). Returns a
   pointer just past the closing apostrophe. */
static const char *scanQuotedString(const char *s, char *buf, int bufsize)
{
    int n = 0;
    s++; /* skip opening quote */
    for (;;) {
        if (*s == '\0') break;
        if (*s == '\'') {
            if (s[1] == '\'') {
                if (buf && n < bufsize-1) buf[n++] = '\'';
                s += 2;
                continue;
            }
            s++;
            break;
        }
        if (buf && n < bufsize-1) buf[n++] = *s;
        s++;
    }
    if (buf) buf[n] = '\0';
    return s;
}

static int isnamestart(int c){ return isalpha((unsigned char)c); }
static int isnamechar(int c){ return isalnum((unsigned char)c); }

static void readName(char **p, char *buf)
{
    int n = 0;
    while (isnamechar((unsigned char)**p)) {
        if (n < 7) buf[n++] = (char)toupper((unsigned char)**p);
        (*p)++;
    }
    buf[n] = '\0';
}

/* returns implicit type from first letter: 0=int,1=real */
static int implicitType(const char *name)
{
    char c = name[0];
    if (c >= 'I' && c <= 'N') return 0;
    return 1;
}

static VAR *findvar(const char *name)
{
    int i;
    for (i = 0; i < nvars; i++)
        if (strcmp(vars[i].name, name) == 0) return &vars[i];
    return NULL;
}

/* Like findvar, but also checks the current call frame's parameter
   bindings first. Needed anywhere that branches on "does this name
   already refer to something" rather than falling through to
   getvar() -- a formal parameter aliased to a caller's array or
   variable isn't in the global vars[] table at all, so plain
   findvar() would wrongly say it doesn't exist. */
static VAR *findvarAliased(const char *name)
{
    if (ncallstack > 0) {
        CALLFRAME *cf = &callstack[ncallstack-1];
        int i;
        for (i=0;i<cf->nbinds;i++)
            if (!strcmp(cf->binds[i].formalName, name)) return cf->binds[i].actual;
    }
    return findvar(name);
}

static VAR *getvar(const char *name)
{
    if (ncallstack > 0) {
        CALLFRAME *cf = &callstack[ncallstack-1];
        int i;
        for (i=0;i<cf->nbinds;i++)
            if (!strcmp(cf->binds[i].formalName, name)) return cf->binds[i].actual;
    }
    {
        VAR *v = findvar(name);
        if (v) return v;
        if (nvars >= MAXVARS) { fatal("out of memory"); return &vars[0]; }
        v = &vars[nvars++];
        strncpy(v->name, name, 7); v->name[7] = 0;
        v->type = implicitType(name);
        v->isArray = 0; v->ndims = 0; v->size = 0;
        v->iarr = NULL; v->rarr = NULL;
        v->ival = 0; v->rval = 0.0f;
        v->carr = NULL; v->cval = NULL; v->clen = 0;
        v->lo[0] = v->lo[1] = v->lo[2] = 1;
        return v;
    }
}

static VAR *declareArray(const char *name, int ndims, int *dims, int *los, int forceType, int hasType)
{
    long size; int k;
    VAR *v = findvar(name);
    if (!v) { v = getvar(name); }
    size = 1;
    for (k = 0; k < ndims; k++) size *= dims[k];
    v->ndims = ndims;
    for (k = 0; k < ndims; k++) { v->dims[k] = dims[k]; v->lo[k] = los ? los[k] : 1; }
    if (hasType) v->type = forceType;
    if (v->isArray && (v->iarr || v->rarr || v->carr)) {
        /* Already-allocated array -- either an earlier DIMENSION in
           this same scope, or (critically) a formal parameter that's
           actually an alias to the caller's already-allocated actual
           argument. Update the dimension metadata used for subscript
           arithmetic, but never reallocate: that would silently wipe
           the caller's data out from under a by-reference parameter. */
        v->size = size;
        return v;
    }
    v->isArray = 1;
    v->size = size;
    if (v->type == 0) v->iarr = (long*)calloc((size_t)size, sizeof(long));
    else               v->rarr = (float*)calloc((size_t)size, sizeof(float));
    return v;
}

/* Parses one dimension specifier in a declaration's dimension list:
   either a bare "upper" bound (FORTRAN's default lower bound of 1),
   or an explicit FORTRAN 77 "lower:upper". Returns the dimension's
   SIZE (upper-lower+1) and reports the lower bound via *loOut, for
   correct subscript-offset arithmetic later in arrIndex(). This is
   only valid in declarations -- an actual array reference/subscript
   (e.g. GRAPH(ROW,COL)) never uses lower:upper, just a single index
   per dimension, so this parser is never used there. */
static int parseDimSpec(char **p, int *loOut)
{
    VAL a = parseExprTop(p);
    long lo, hi;
    skipsp(p);
    if (**p==':') {
        VAL b;
        (*p)++;
        b = parseExprTop(p);
        lo = VI(a); hi = VI(b);
    } else {
        lo = 1; hi = VI(a);
    }
    *loOut = (int)lo;
    return (int)(hi - lo + 1);
}

/* Parses a whole "(d1,d2,d3)" dimension list using parseDimSpec for
   each entry. Returns the dimension count. */
static int parseDimList(char **p, int *sizes, int *los, int max)
{
    int n = 0;
    skipsp(p);
    if (**p != '(') { fatal("syntax error"); return 0; }
    (*p)++;
    skipsp(p);
    if (**p==')') { (*p)++; return 0; }
    for (;;) {
        int lo, sz;
        sz = parseDimSpec(p, &lo);
        if (n < max) { sizes[n]=sz; los[n]=lo; n++; }
        skipsp(p);
        if (**p==',') { (*p)++; continue; }
        if (**p==')') { (*p)++; break; }
        fatal("syntax error"); break;
    }
    return n;
}

static long arrIndex(VAR *v, long i1, long i2, long i3)
{
    long idx = 0;
    if (v->ndims == 1) idx = i1 - v->lo[0];
    else if (v->ndims == 2) idx = (i1 - v->lo[0]) + v->dims[0] * (i2 - v->lo[1]);
    else idx = (i1 - v->lo[0]) + v->dims[0] * ((i2 - v->lo[1]) + v->dims[1] * (i3 - v->lo[2]));
    if (idx < 0 || idx >= v->size) { fatal("subscript out of range"); idx = 0; }
    return idx;
}

/* ------------------------------------------------------------------
   expression parser (recursive descent, operates on a char**)
   ------------------------------------------------------------------ */

static int isIntrinsic(const char *nm)
{
    static const char *list[] = {
        "SIN","COS","TAN","ATAN","EXP","ALOG","ALOG10","SQRT",
        "ABS","IABS","FLOAT","IFIX","INT","MOD","AMOD","SIGN",
        "MAX0","MAX1","AMAX0","AMAX1","MIN0","MIN1","AMIN0","AMIN1","MAX","MIN",0
    };
    int i;
    for (i = 0; list[i]; i++) if (strcmp(list[i], nm) == 0) return 1;
    return 0;
}

static VAL callIntrinsic(const char *nm, VAL *a, int na)
{
    if (na < 1) { fatal("illegal function call"); return mkint(0); }
    if (!strcmp(nm,"SIN"))    return mkreal((float)sin((double)VF(a[0])));
    if (!strcmp(nm,"COS"))    return mkreal((float)cos((double)VF(a[0])));
    if (!strcmp(nm,"TAN"))    return mkreal((float)tan((double)VF(a[0])));
    if (!strcmp(nm,"ATAN"))   return mkreal((float)atan((double)VF(a[0])));
    if (!strcmp(nm,"EXP"))    return mkreal((float)exp((double)VF(a[0])));
    if (!strcmp(nm,"ALOG"))   return mkreal((float)log((double)VF(a[0])));
    if (!strcmp(nm,"ALOG10")) return mkreal((float)log10((double)VF(a[0])));
    if (!strcmp(nm,"SQRT"))   return mkreal((float)sqrt((double)VF(a[0])));
    if (!strcmp(nm,"ABS"))    return mkreal((float)fabs((double)VF(a[0])));
    if (!strcmp(nm,"IABS"))   return mkint(labs(VI(a[0])));
    if (!strcmp(nm,"FLOAT"))  return mkreal((float)VI(a[0]));
    if (!strcmp(nm,"IFIX"))   return mkint((long)VF(a[0]));
    if (!strcmp(nm,"INT"))    return mkint((long)VF(a[0]));
    if (!strcmp(nm,"MOD"))    return mkint(na>1 ? VI(a[0]) % (VI(a[1])==0?1:VI(a[1])) : 0);
    if (!strcmp(nm,"AMOD"))   return mkreal(na>1 ? (float)fmod((double)VF(a[0]),(double)VF(a[1])) : 0.0f);
    if (!strcmp(nm,"SIGN"))   { float m=(float)fabs((double)VF(a[0])); return mkreal(na>1 ? (VF(a[1])<0 ? -m : m) : VF(a[0])); }
    if (!strncmp(nm,"MAX",3) || !strncmp(nm,"AMAX",4)) {
        float m = VF(a[0]); int ii;
        for (ii=1; ii<na; ii++) if (VF(a[ii]) > m) m = VF(a[ii]);
        if (!strcmp(nm,"MAX0")) return mkint((long)m);
        return mkreal(m);
    }
    if (!strncmp(nm,"MIN",3) || !strncmp(nm,"AMIN",4)) {
        float m = VF(a[0]); int ii;
        for (ii=1; ii<na; ii++) if (VF(a[ii]) < m) m = VF(a[ii]);
        if (!strcmp(nm,"MIN0")) return mkint((long)m);
        return mkreal(m);
    }
    fatal("undefined function"); return mkint(0);
}

/* parses "(e1,e2,...)" already positioned at '(' ; returns count, fills arr */
static int parseArgList(char **p, VAL *arr, int max)
{
    int n = 0;
    skipsp(p);
    if (**p != '(') { fatal("syntax error"); return 0; }
    (*p)++;
    skipsp(p);
    if (**p == ')') { (*p)++; return 0; }
    for (;;) {
        if (n >= max) { fatal("syntax error"); }
        arr[n++] = parseExprTop(p);
        skipsp(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ')') { (*p)++; break; }
        fatal("syntax error"); break;
    }
    return n;
}

/* ------------------------------------------------------------------
   Pseudo-object / execution listing.

   IV-TRAN is an interpreter, not a compiler, so there is no real
   machine object code to show. In the spirit of the classic
   compiler listings this project is meant to evoke, when LISTING
   is turned on every executed statement is echoed with its label
   and source text, followed by a trace of simple stack-machine
   style pseudo-instructions (LIT/LD/STO/ADD/CALL/JMP/...) showing,
   in execution order, what the interpreter actually did to carry
   it out -- an honest "object listing" of the interpreter's own
   operations, complete with a synthetic incrementing address.
   ------------------------------------------------------------------ */
static int  listMode = 0;
static int  traceMode = 0;
static long pcAddr = 0;

static void emitOp(const char *mnem, const char *operand)
{
    if (!traceMode) return;
    if (operand && operand[0])
        printf(" %04lX  %-6s %s\n", pcAddr, mnem, operand);
    else
        printf(" %04lX  %s\n", pcAddr, mnem);
    pcAddr += 2;
}

/* Wraps a string operand in quotes for the listing, so an empty or
   all-blank literal (e.g. the Hollerith space in plot3.f's DATA
   statement) is visibly bounded rather than looking like nothing
   was emitted at all. Display only -- does not affect parsing. */
static void emitOpQuoted(const char *mnem, const char *text)
{
    char buf[80];
    if (!traceMode) return;
    buf[0] = '\'';
    strncpy(buf+1, text, sizeof(buf)-3);
    buf[sizeof(buf)-2] = 0;
    strcat(buf, "'");
    emitOp(mnem, buf);
}

static void listStatementHeader(long label, const char *text)
{
    if (!traceMode) return;
    if (label) printf("                  %5ld  %s\n", label, text);
    else        printf("                         %s\n", text);
}

static VAL parsePrimary(char **p)
{
    VAL v;
    skipsp(p);
    if (**p == '(') {
        (*p)++;
        v = parseConcatExpr(p);
        skipsp(p);
        if (**p == ')') (*p)++; else fatal("syntax error");
        return v;
    }
    if (**p == '\'') {
        char buf[64];
        *p = (char*)scanQuotedString(*p, buf, sizeof(buf));
        emitOpQuoted("LITS", buf);
        return mkstr(buf);
    }
    if (isdigit((unsigned char)**p) || **p=='.') {
        char buf[64]; int n=0; int isreal=0;
        while (isdigit((unsigned char)**p)) buf[n++]=*(*p)++;
        /* Hollerith?  nH.... */
        if ((**p=='H' || **p=='h') && n>0) {
            int cnt = atoi(buf); int i; long code=0; char first=0;
            char disp[2];
            (*p)++; /* skip H */
            for (i=0;i<cnt;i++) { if (i==0) first = **p; (*p)++; }
            code = (unsigned char)first;
            disp[0] = first; disp[1] = 0;
            emitOpQuoted("LITH", disp);
            return mkint(code);
        }
        if (**p=='.') { buf[n++]=*(*p)++; isreal=1;
            while (isdigit((unsigned char)**p)) buf[n++]=*(*p)++; }
        if (**p=='E' || **p=='e') { isreal=1; buf[n++]=*(*p)++;
            if (**p=='+'||**p=='-') buf[n++]=*(*p)++;
            while (isdigit((unsigned char)**p)) buf[n++]=*(*p)++; }
        buf[n]=0;
        emitOp("LIT", buf);
        if (isreal) return mkreal((float)atof(buf));
        return mkint(atol(buf));
    }
    if (isnamestart((unsigned char)**p)) {
        char nm[8]; VAR *var;
        readName(p, nm);
        skipsp(p);
        if (**p=='(') {
            if (isIntrinsic(nm)) {
                VAL args[8]; int na = parseArgList(p, args, 8);
                emitOp("CALL", nm);
                return callIntrinsic(nm, args, na);
            }
            {
                STMTFUNC *sf = findStmtFunc(nm);
                if (sf) {
                    VAL args[MAXSFARGS]; int na = parseArgList(p, args, MAXSFARGS);
                    VAL saved[MAXSFARGS]; VAR *av[MAXSFARGS];
                    VAL result; char *bp; int ai;
                    for (ai=0; ai<sf->nargs; ai++) {
                        av[ai] = getvar(sf->args[ai]);
                        if (av[ai]->type==0) saved[ai]=mkint(av[ai]->ival);
                        else saved[ai]=mkreal(av[ai]->rval);
                        if (ai<na) {
                            if (av[ai]->type==0) av[ai]->ival = VI(args[ai]);
                            else av[ai]->rval = VF(args[ai]);
                        }
                    }
                    bp = sf->body;
                    emitOp("CALL", nm);
                    result = parseExprTop(&bp);
                    for (ai=0; ai<sf->nargs; ai++) {
                        if (av[ai]->type==0) av[ai]->ival = VI(saved[ai]);
                        else av[ai]->rval = VF(saved[ai]);
                    }
                    return result;
                }
            }
            {
                VAL subs[3]; int ns; long i1=1,i2=1,i3=1;
                var = getvar(nm);
                ns = parseArgList(p, subs, 3);
                if (ns>=1) i1 = VI(subs[0]);
                if (ns>=2) i2 = VI(subs[1]);
                if (ns>=3) i3 = VI(subs[2]);
                if (!var->isArray) { fatal("array not dimensioned"); return mkint(0); }
                {
                    long idx = arrIndex(var, i1, i2, i3);
                    emitOp("LDX", nm);
                    if (var->type==2) {
                        char buf[128]; int n = var->clen; if (n>127) n=127;
                        memcpy(buf, var->carr + idx*(long)var->clen, (size_t)n);
                        buf[n]=0;
                        return mkstr(buf);
                    }
                    if (var->type==0) return mkint(var->iarr[idx]);
                    else return mkreal(var->rarr[idx]);
                }
            }
        } else {
            var = getvar(nm);
            if (var->isArray) { fatal("syntax error"); return mkint(0); }
            emitOp("LD", nm);
            if (var->type==2) return mkstr(var->cval ? var->cval : "");
            if (var->type==0) return mkint(var->ival);
            else return mkreal(var->rval);
        }
    }
    fatal("syntax error");
    return mkint(0);
}

static VAL parseUnary(char **p)
{
    skipsp(p);
    if (**p=='-') { VAL v; (*p)++; v=parseUnary(p);
        return v.isReal ? mkreal(-v.r) : mkint(-v.i); }
    if (**p=='+') { (*p)++; return parseUnary(p); }
    return parsePrimary(p);
}

static VAL binop(char op, VAL a, VAL b)
{
    int real = a.isReal || b.isReal;
    VAL result;
    if (op=='+') result = real ? mkreal(VF(a)+VF(b)) : mkint(VI(a)+VI(b));
    else if (op=='-') result = real ? mkreal(VF(a)-VF(b)) : mkint(VI(a)-VI(b));
    else if (op=='*') result = real ? mkreal(VF(a)*VF(b)) : mkint(VI(a)*VI(b));
    else if (op=='/') {
        if (real) result = mkreal(VF(a)/VF(b));
        else if (VI(b)==0) { fatal("division by zero"); return mkint(0); }
        else result = mkint(VI(a)/VI(b));
    }
    else return mkint(0);
    emitOp(op=='+'?"ADD":op=='-'?"SUB":op=='*'?"MUL":"DIV", NULL);
    return result;
}

static VAL parsePower(char **p)
{
    VAL base = parseUnary(p);
    skipsp(p);
    if ((*p)[0]=='*' && (*p)[1]=='*') {
        VAL exp; VAL result;
        (*p) += 2;
        exp = parsePower(p); /* right assoc */
        if (!exp.isReal && exp.i>=0 && !base.isReal) {
            long r=1, b=base.i, n=exp.i; while(n-->0) r*=b; result = mkint(r);
        } else {
            result = mkreal((float)pow((double)VF(base),(double)VF(exp)));
        }
        emitOp("PWR", NULL);
        return result;
    }
    return base;
}

static VAL parseTerm(char **p)
{
    VAL v = parsePower(p);
    for (;;) {
        skipsp(p);
        if (**p=='*' && (*p)[1]!='*') { (*p)++; v=binop('*', v, parsePower(p)); }
        else if (**p=='/' && (*p)[1]!='/') { (*p)++; v=binop('/', v, parsePower(p)); }
        else break;
    }
    return v;
}

static VAL parseExprTop(char **p)
{
    VAL v = parseTerm(p);
    for (;;) {
        skipsp(p);
        if (**p=='+') { (*p)++; v=binop('+', v, parseTerm(p)); }
        else if (**p=='-') { (*p)++; v=binop('-', v, parseTerm(p)); }
        else break;
    }
    return v;
}

static VAL concatVals(VAL a, VAL b)
{
    char buf[128], sa[64], sb[64];
    int la, lb, i, n;
    if (a.isStr) { strncpy(sa,a.s,63); sa[63]=0; } else { sprintf(sa,"%ld",VI(a)); }
    if (b.isStr) { strncpy(sb,b.s,63); sb[63]=0; } else { sprintf(sb,"%ld",VI(b)); }
    la = (int)strlen(sa); lb = (int)strlen(sb);
    if (la > 127) la = 127;
    for (i=0;i<la;i++) buf[i] = sa[i];
    n = la;
    for (i=0;i<lb && n<127;i++) buf[n++] = sb[i];
    buf[n] = 0;
    emitOp("CAT", NULL);
    return mkstr(buf);
}

/* string concatenation, // -- binds looser than arithmetic, so this
   sits one level above parseExprTop */
static VAL parseConcatExpr(char **p)
{
    VAL v = parseExprTop(p);
    skipsp(p);
    while ((*p)[0]=='/' && (*p)[1]=='/') {
        VAL b;
        (*p) += 2;
        b = parseExprTop(p);
        v = concatVals(v, b);
        skipsp(p);
    }
    return v;
}

/* --- relational / logical, used by IF --- */
static int matchDot(char **p, const char *word)
{
    size_t n = strlen(word);
    size_t i;
    if ((*p)[0] != '.') return 0;
    for (i=0;i<n;i++) {
        if (toupper((unsigned char)(*p)[1+i]) != (unsigned char)word[i]) return 0;
    }
    if ((*p)[1+n] != '.') return 0;
    *p += n+2;
    return 1;
}

static int cmpVals(VAL a, VAL b)
{
    if (a.isStr || b.isStr) {
        char sa[128], sb[128]; int la, lb, maxlen, i;
        strncpy(sa, a.isStr?a.s:"", 127); sa[127]=0;
        strncpy(sb, b.isStr?b.s:"", 127); sb[127]=0;
        la=(int)strlen(sa); lb=(int)strlen(sb);
        maxlen = (la>lb?la:lb); if (maxlen>127) maxlen=127;
        for (i=la;i<maxlen;i++) sa[i]=' '; sa[maxlen]=0;
        for (i=lb;i<maxlen;i++) sb[i]=' '; sb[maxlen]=0;
        return strcmp(sa,sb);
    }
    { float fa=VF(a), fb=VF(b); if (fa<fb) return -1; if (fa>fb) return 1; return 0; }
}

static VAL parseRelational(char **p)
{
    VAL a = parseConcatExpr(p);
    skipsp(p);
    if (matchDot(p,"LT")) { VAL b=parseConcatExpr(p); VAL r=mkint(cmpVals(a,b)< 0); emitOp("CMP","LT"); return r; }
    if (matchDot(p,"LE")) { VAL b=parseConcatExpr(p); VAL r=mkint(cmpVals(a,b)<=0); emitOp("CMP","LE"); return r; }
    if (matchDot(p,"EQ")) { VAL b=parseConcatExpr(p); VAL r=mkint(cmpVals(a,b)==0); emitOp("CMP","EQ"); return r; }
    if (matchDot(p,"NE")) { VAL b=parseConcatExpr(p); VAL r=mkint(cmpVals(a,b)!=0); emitOp("CMP","NE"); return r; }
    if (matchDot(p,"GT")) { VAL b=parseConcatExpr(p); VAL r=mkint(cmpVals(a,b)> 0); emitOp("CMP","GT"); return r; }
    if (matchDot(p,"GE")) { VAL b=parseConcatExpr(p); VAL r=mkint(cmpVals(a,b)>=0); emitOp("CMP","GE"); return r; }
    return a;
}

static VAL parseNot(char **p)
{
    skipsp(p);
    if (matchDot(p,"NOT")) { VAL v = parseNot(p); return mkint(!VI(v)); }
    return parseRelational(p);
}

static VAL parseAnd(char **p)
{
    VAL v = parseNot(p);
    skipsp(p);
    while (matchDot(p,"AND")) { VAL b=parseNot(p); v = mkint(VI(v)&&VI(b)); skipsp(p); }
    return v;
}

static VAL evalLogical(char **p)
{
    VAL v = parseAnd(p);
    skipsp(p);
    while (matchDot(p,"OR")) { VAL b=parseAnd(p); v = mkint(VI(v)||VI(b)); skipsp(p); }
    return v;
}

/* ------------------------------------------------------------------
   assignment target resolution + store
   ------------------------------------------------------------------ */

/* Copies a VAL's text into a fixed-length CHARACTER buffer, per
   ANSI assignment rules: truncate if too long, blank-pad if short.
   Numeric values (unusual, but not illegal) are rendered as text. */
static void putCharToBuf(char *dst, int clen, VAL v)
{
    char numbuf[32];
    const char *src;
    int i, srclen;
    if (v.isStr) src = v.s;
    else { sprintf(numbuf, v.isReal ? "%g" : "%ld", v.isReal ? (double)v.r : (double)v.i); src = numbuf; }
    srclen = (int)strlen(src);
    for (i=0; i<clen; i++) dst[i] = (i<srclen) ? src[i] : ' ';
}

static void storeInto(char *name, int hasSub, long i1,long i2,long i3, VAL v)
{
    VAR *var = getvar(name);
    if (hasSub) {
        long idx;
        if (!var->isArray) { fatal("array not dimensioned"); return; }
        idx = arrIndex(var, i1,i2,i3);
        if (var->type==2) putCharToBuf(var->carr + idx*(long)var->clen, var->clen, v);
        else if (var->type==0) var->iarr[idx] = VI(v);
        else var->rarr[idx] = VF(v);
        emitOp("STOX", name);
    } else {
        if (var->isArray) { fatal("syntax error"); return; }
        if (var->type==2) putCharToBuf(var->cval, var->clen, v);
        else if (var->type==0) var->ival = VI(v);
        else var->rval = VF(v);
        emitOp("STO", name);
    }
}

/* ------------------------------------------------------------------
   output list evaluation (handles nested implied-DO)
   ------------------------------------------------------------------ */
static VAL FARBIG outbuf[MAXVALS];
static int  noutbuf;

static void parseOutItem(char **p);

static void parseOutList(char **p, char term)
{
    for (;;) {
        parseOutItem(p);
        skipsp(p);
        if (**p==',') { (*p)++; continue; }
        break;
    }
    (void)term;
}

/* advance past one syntactic "item" (balancing parens, skipping quoted
   text) with NO evaluation side effects; stops at a top-level comma or
   the closing paren of the enclosing group. */
static char *skipItemText(char *s)
{
    int depth = 0;
    for (;;) {
        if (*s == '\0') break;
        if (*s == '\'') { s = (char*)scanQuotedString(s, NULL, 0); continue; }
        if (*s == '(') { depth++; s++; continue; }
        if (*s == ')') { if (depth==0) break; depth--; s++; continue; }
        if (*s == ',' && depth==0) break;
        s++;
    }
    return s;
}

#define MAXSEG 16

/* attempt to parse a parenthesised group as an implied-DO;
   if it isn't one, fall back to treating it as a plain expression. */
static void parseOutItem(char **p)
{
    skipsp(p);
    if (**p=='(') {
        char *save = *p;
        char *segStart[MAXSEG];
        char *segEnd[MAXSEG];
        int nseg = 0;
        char *r = save + 1;
        int k = -1, i;

        /* Phase 1: split into top-level comma-separated segments, purely
           by scanning (no expression evaluation, no side effects). */
        for (;;) {
            char *e;
            skipsp(&r);
            if (nseg >= MAXSEG) break;
            segStart[nseg] = r;
            e = skipItemText(r);
            segEnd[nseg] = e;
            nseg++;
            if (*e == ',') { r = e + 1; continue; }
            r = e; /* points at ')' or '\0' */
            break;
        }

        /* Phase 2: find the first segment that looks like "IDENT =" --
           that marks the start of the control clause. */
        for (i = 0; i < nseg; i++) {
            char *q = segStart[i];
            if (isnamestart((unsigned char)*q)) {
                char nm[8]; char *look = q;
                readName(&look, nm);
                skipsp(&look);
                if (*look == '=' && look < segEnd[i]) { k = i; break; }
            }
        }

        if (k < 0 || k + 1 >= nseg) {
            /* Not an implied DO: treat the whole (...) as one expression. */
            VAL v;
            *p = save;
            v = parseExprTop(p);
            if (noutbuf < MAXVALS) outbuf[noutbuf++] = v;
            return;
        }

        /* Phase 3: evaluate. Segments [0..k-1] are the repeated item
           list; segment k is "NAME=e1"; segment k+1 is e2; optional
           segment k+2 is e3. */
        {
            char nm[8]; char *look = segStart[k];
            VAL e1, e2, e3; long lo, hi, st, ii;
            VAR *lv; int isReal; int nitems = k; int it;

            readName(&look, nm);
            skipsp(&look);
            look++; /* skip '=' */
            e1 = parseExprTop(&look);
            { char *q2 = segStart[k+1]; e2 = parseExprTop(&q2); }
            e3 = mkint(1);
            if (k + 2 < nseg) { char *q3 = segStart[k+2]; e3 = parseExprTop(&q3); }

            lv = getvar(nm); isReal = lv->type;
            lo = VI(e1); hi = VI(e2); st = VI(e3);
            if (st == 0) st = 1;
            for (ii = lo; (st > 0) ? (ii <= hi) : (ii >= hi); ii += st) {
                if (isReal) lv->rval = (float)ii; else lv->ival = ii;
                for (it = 0; it < nitems; it++) {
                    char *ip = segStart[it];
                    parseOutItem(&ip);
                }
            }
        }
        *p = r;
        if (**p == ')') (*p)++;
        return;
    }
    if (isnamestart((unsigned char)**p)) {
        char *look = *p; char nm[8];
        readName(&look, nm);
        skipsp(&look);
        if (*look != '(') {
            VAR *var = findvarAliased(nm);
            if (var && var->isArray) {
                long k;
                for (k=0; k<var->size; k++) {
                    VAL v;
                    if (var->type==2) {
                        char buf[128]; int n = var->clen; if (n>127) n=127;
                        memcpy(buf, var->carr + k*(long)var->clen, (size_t)n);
                        buf[n]=0;
                        v = mkstr(buf);
                    } else {
                        v = (var->type==0) ? mkint(var->iarr[k]) : mkreal(var->rarr[k]);
                    }
                    if (noutbuf < MAXVALS) outbuf[noutbuf++] = v;
                }
                *p = look;
                return;
            }
        }
    }
    {
        VAL v = parseConcatExpr(p);
        if (noutbuf < MAXVALS) outbuf[noutbuf++] = v;
    }
}

/* ------------------------------------------------------------------
   FORMAT application

   ANSI FORTRAN carriage control: the FIRST character of a formatted
   output record is not data -- it is a control code telling the
   printer how to space before printing the rest of the record:
       ' '  normal single-space advance (the common case)
       '0'  double-space (skip a blank line first)
       '1'  new page
       '+'  overprint -- no line advance at all
   Any other character is treated the same as ' ' (implementation
   defined, but "just advance normally" is the universal behavior).
   We build each record into a buffer first so the leading character
   can be inspected and stripped before anything is actually written.
   ------------------------------------------------------------------ */
#define RECBUFLEN LINELEN
static char recbuf[RECBUFLEN];
static int  reclen;

static void recPutc(int c)
{
    if (reclen < RECBUFLEN-1) recbuf[reclen++] = (char)c;
}
static void recPuts(const char *s)
{
    while (*s) recPutc(*s++);
}

static void emitRecord(void)
{
    char ctrl = (reclen>0) ? recbuf[0] : ' ';
    int start = (reclen>0) ? 1 : 0;
    if (ctrl=='0') putchar('\n');       /* double space: extra blank line */
    else if (ctrl=='1') putchar('\f');  /* new page */
    else if (ctrl=='+') putchar('\r');  /* overprint: return, no advance */
    { int i; for (i=start; i<reclen; i++) putchar(recbuf[i]); }
    if (ctrl!='+') putchar('\n');
    reclen = 0;
}

static void putpad(const char *s, int width)
{
    int len = (int)strlen(s);
    int pad = width - len;
    if (pad < 0) pad = 0;
    while (pad-- > 0) recPutc(' ');
    recPuts(s);
}

/* processes fmt string (without outer parens) against outbuf[vi..],
   returns index of next value consumed. Recursively handles groups
   and repeat counts. Returns 1 if a data descriptor was emitted
   (used to detect whether reversion should continue). */
static void putAField(VAL v, int w)
{
    char buf[64]; int len, i;
    if (v.isStr) { strncpy(buf, v.s, sizeof(buf)-1); buf[sizeof(buf)-1]=0; }
    else { buf[0] = (char)(VI(v) & 0xFF); buf[1] = 0; }
    len = (int)strlen(buf);
    if (w <= 0) w = (len > 0) ? len : 1;
    for (i = 0; i < w; i++) recPutc(i < len ? buf[i] : ' ');
}

static int applyFormatItems(const char *fmt, int *fp, int len, int *vi, int nv);

static int applyGroupRepeat(const char *fmt, int *fp, int len, int rep, int *vi, int nv)
{
    int k; int any=0;
    int groupStart = *fp;
    int groupEnd, depth=0, q=groupStart;
    /* pre-scan to find this group's own matching ')', so we know
       where to resume in the caller regardless of early exhaustion
       or how many repetitions actually ran. */
    while (q<len) {
        if (fmt[q]=='(') depth++;
        else if (fmt[q]==')') { if (depth==0) break; depth--; }
        q++;
    }
    groupEnd = q+1;
    for (k=0;k<rep;k++) {
        int r;
        *fp = groupStart;
        r = applyFormatItems(fmt, fp, len, vi, nv);
        any = any || r;
        if (*vi >= nv && r) break; /* exhausted values mid-group */
    }
    *fp = groupEnd;
    return any;
}

static int applyFormatItems(const char *fmt, int *fp, int len, int *vi, int nv)
{
    int emittedData = 0;
    while (*fp < len) {
        int c = fmt[*fp];
        int n = 1;
        if (c==' ' || c==',') { (*fp)++; continue; }
        if (c==')') { (*fp)++; break; }
        if (c=='(') {
            (*fp)++;
            applyFormatItems(fmt, fp, len, vi, nv);
            continue;
        }
        if (c=='\'') {
            char buf[LINELEN];
            const char *after = scanQuotedString(&fmt[*fp], buf, sizeof(buf));
            recPuts(buf);
            *fp += (int)(after - &fmt[*fp]);
            continue;
        }
        if (c=='/') { (*fp)++; emitRecord(); continue; }
        if (c=='T' || c=='t') {
            int col; char nb[16]; int nn=0;
            (*fp)++;
            while (isdigit((unsigned char)fmt[*fp]) && nn<15) nb[nn++]=fmt[(*fp)++];
            nb[nn]=0; col = atoi(nb);
            while (reclen < col-1) recPutc(' ');
            continue;
        }
        if (isdigit((unsigned char)c)) {
            char numbuf[16]; int nn=0;
            while (isdigit((unsigned char)fmt[*fp]) && nn<15) numbuf[nn++]=fmt[(*fp)++];
            numbuf[nn]=0; n = atoi(numbuf);
            if (fmt[*fp]=='H' || fmt[*fp]=='h') {
                int i; (*fp)++;
                for (i=0;i<n;i++) { if (fmt[*fp]) recPutc(fmt[(*fp)++]); }
                continue;
            }
            if (fmt[*fp]=='(') {
                (*fp)++;
                applyGroupRepeat(fmt, fp, len, n, vi, nv);
                continue;
            }
            /* otherwise n is a repeat count and fmt[*fp] is now the
               descriptor letter (I/F/E/A/X) -- fall through below */
        }
        /* descriptor letter, with repeat count n (defaults to 1 if no
           leading digit was present, e.g. plain "F10.6") */
        {
            char kind = fmt[*fp];
            if (kind=='X' || kind=='x') {
                int i; (*fp)++;
                for (i=0;i<n;i++) recPutc(' ');
                continue;
            }
            if (kind=='I'||kind=='i') {
                int w; (*fp)++;
                { char nb[16]; int nnn=0; while(isdigit((unsigned char)fmt[*fp])&&nnn<15) nb[nnn++]=fmt[(*fp)++]; nb[nnn]=0; w=atoi(nb); if(w==0) w=6; }
                {
                    int rep;
                    for (rep=0; rep<n; rep++) {
                        char b[32];
                        if (*vi>=nv) return emittedData;
                        sprintf(b,"%ld", VI(outbuf[*vi])); (*vi)++;
                        putpad(b, w); emittedData=1;
                    }
                }
                continue;
            }
            if (kind=='F'||kind=='f') {
                int w,d; (*fp)++;
                { char nb[16]; int nnn=0; while(isdigit((unsigned char)fmt[*fp])&&nnn<15) nb[nnn++]=fmt[(*fp)++]; nb[nnn]=0; w=atoi(nb); if(w==0) w=10; }
                d=2;
                if (fmt[*fp]=='.') { char nb[16]; int nnn=0; (*fp)++; while(isdigit((unsigned char)fmt[*fp])&&nnn<15) nb[nnn++]=fmt[(*fp)++]; nb[nnn]=0; d=atoi(nb); }
                {
                    int rep;
                    for (rep=0; rep<n; rep++) {
                        char b[40], fmtb[16];
                        if (*vi>=nv) return emittedData;
                        sprintf(fmtb, "%%.%df", d);
                        sprintf(b, fmtb, (double)VF(outbuf[*vi])); (*vi)++;
                        putpad(b, w); emittedData=1;
                    }
                }
                continue;
            }
            if (kind=='E'||kind=='e') {
                int w,d; (*fp)++;
                { char nb[16]; int nnn=0; while(isdigit((unsigned char)fmt[*fp])&&nnn<15) nb[nnn++]=fmt[(*fp)++]; nb[nnn]=0; w=atoi(nb); if(w==0) w=12; }
                d=4;
                if (fmt[*fp]=='.') { char nb[16]; int nnn=0; (*fp)++; while(isdigit((unsigned char)fmt[*fp])&&nnn<15) nb[nnn++]=fmt[(*fp)++]; nb[nnn]=0; d=atoi(nb); }
                {
                    int rep;
                    for (rep=0; rep<n; rep++) {
                        char b[40], fmtb[16];
                        if (*vi>=nv) return emittedData;
                        sprintf(fmtb, "%%.%de", d);
                        sprintf(b, fmtb, (double)VF(outbuf[*vi])); (*vi)++;
                        putpad(b, w); emittedData=1;
                    }
                }
                continue;
            }
            if (kind=='A'||kind=='a') {
                int w; (*fp)++;
                { char nb[16]; int nnn=0; while(isdigit((unsigned char)fmt[*fp])&&nnn<15) nb[nnn++]=fmt[(*fp)++]; nb[nnn]=0; w=atoi(nb); }
                {
                    int rep;
                    for (rep=0; rep<n; rep++) {
                        if (*vi>=nv) return emittedData;
                        putAField(outbuf[*vi], w); (*vi)++;
                        emittedData=1;
                    }
                }
                continue;
            }
            /* unknown descriptor letter: skip it so we don't loop forever */
            (*fp)++;
            continue;
        }
    }
    return emittedData;
}

static void applyFormat(const char *rawfmt, int nv)
{
    /* strip outer parens if present */
    char buf[LINELEN]; int len; const char *s = rawfmt;
    int fp=0; int vi=0;
    skipsp((char**)&s);
    if (*s=='(') s++;
    strncpy(buf, s, LINELEN-1); buf[LINELEN-1]=0;
    len = (int)strlen(buf);
    if (len>0 && buf[len-1]==')') buf[--len]=0;
    if (nv==0) {
        /* still emit literal text once */
        fp=0; reclen=0;
        applyFormatItems(buf,&fp,len,&vi,nv);
        emitRecord();
        return;
    }
    while (vi < nv) {
        int before = vi;
        fp = 0; reclen = 0;
        applyFormatItems(buf, &fp, len, &vi, nv);
        emitRecord();
        if (vi == before) break; /* no progress -> avoid infinite loop */
    }
}

/* ------------------------------------------------------------------
   list-directed (PRINT *,  / no FORMAT) output
   ------------------------------------------------------------------ */
static void listDirectedPrint(int nv)
{
    int i;
    for (i=0;i<nv;i++) {
        if (outbuf[i].isStr) printf(" %s", outbuf[i].s);
        else if (outbuf[i].isReal) printf(" %14.6f", (double)VF(outbuf[i]));
        else printf(" %10ld", VI(outbuf[i]));
    }
    putchar('\n');
}

/* ------------------------------------------------------------------
   statement recognition helpers
   ------------------------------------------------------------------ */
static int kwis(const char *s, const char *kw)
{
    size_t n = strlen(kw);
    size_t i;
    for (i=0;i<n;i++) {
        if (toupper((unsigned char)s[i]) != (unsigned char)kw[i]) return 0;
    }
    /* next char must not be a name char, so INTEGERX isn't matched as INTEGER */
    if (isnamechar((unsigned char)s[n])) return 0;
    return 1;
}

/* Set when a WRITE/PRINT gives its FORMAT as a quoted string
   constant right inline (PRINT '(T3,99A)', list) instead of a label
   referencing a separate FORMAT statement -- both are legal FORTRAN,
   and the inline form needs its own text carried somewhere other
   than the label table since it was never given one. */
static char inlineFmtBuf[LINELEN];
static int  inlineFmtSet = 0;

static const char *findFormat(long lab)
{
    int i;
    for (i=0;i<nfmts;i++) if (fmts[i].label==lab) return (const char*)fmts[i].text;
    return NULL;
}

/* parse "(unit,label)" -> returns label (unit ignored except detect '*') */
static long parseUnitFmt(char **p, int *isListDirected)
{
    long lab = 0;
    *isListDirected = 0;
    skipsp(p);
    if (**p=='(') {
        (*p)++;
        skipsp(p);
        /* unit */
        while (**p && **p!=',') (*p)++;
        if (**p==',') (*p)++;
        skipsp(p);
        if (**p=='*') { *isListDirected=1; (*p)++; }
        else if (**p=='\'') {
            *p = (char*)scanQuotedString(*p, inlineFmtBuf, sizeof(inlineFmtBuf));
            inlineFmtSet = 1;
        }
        else {
            char nb[16]; int n=0;
            while (isdigit((unsigned char)**p) && n<15) nb[n++]=*(*p)++;
            nb[n]=0; lab = atol(nb);
        }
        skipsp(p);
        if (**p==')') (*p)++;
    }
    return lab;
}

/* DATA name[,name...] / val[,val...] / [name.../val.../] ...
   Handles the common forms:  DATA A/1/  DATA A,B/1,2/
   DATA STAR/'*'/ DASH/'-'/ ...   (groups may be separated by a
   comma or just whitespace, as different source listings vary). */
static void doData(char *p)
{
    for (;;) {
        char names[16][8]; int nn=0;
        VAL vals[16]; int nvv=0;
        int i;
        skipsp(&p);
        if (!isnamestart((unsigned char)*p)) break;
        for (;;) {
            char nm[8];
            skipsp(&p);
            if (!isnamestart((unsigned char)*p)) break;
            readName(&p, nm);
            if (nn < 16) strcpy(names[nn++], nm);
            skipsp(&p);
            if (*p==',') { p++; continue; }
            break;
        }
        skipsp(&p);
        if (*p != '/') { fatal("syntax error"); return; }
        p++;
        for (;;) {
            VAL v;
            skipsp(&p);
            v = parseUnary(&p);
            if (nvv < 16) vals[nvv++] = v;
            skipsp(&p);
            if (*p==',') { p++; continue; }
            break;
        }
        skipsp(&p);
        if (*p=='/') p++;
        for (i=0;i<nn;i++) {
            VAR *var = getvar(names[i]);
            if (var->isArray) {
                long k;
                for (k=0;k<var->size;k++) {
                    VAL vv = vals[ nvv>0 ? (int)(k % nvv) : 0 ];
                    if (var->type==2) putCharToBuf(var->carr + k*(long)var->clen, var->clen, vv);
                    else if (var->type==0) var->iarr[k]=VI(vv);
                    else var->rarr[k]=VF(vv);
                }
            } else {
                VAL v = vals[ nvv>0 ? (i % nvv) : 0 ];
                if (var->type==2) putCharToBuf(var->cval, var->clen, v);
                else if (var->type==0) var->ival = VI(v);
                else var->rval = VF(v);
            }
        }
        skipsp(&p);
        if (*p==',') p++;
        if (*p==0) break;
    }
}

/* PARAMETER (name1 = expr1, name2 = expr2, ...) -- named constants.
   Not enforced as immutable (nothing stops a later assignment from
   changing one), since real programs don't normally do that anyway
   and enforcing it would add bookkeeping for little practical
   benefit here; this just sets each name's value once, respecting
   whatever type it already has from an earlier INTEGER/REAL/
   CHARACTER declaration (or implicit typing if undeclared). */
static void doParameter(char *p)
{
    skipsp(&p);
    if (*p=='(') p++;
    for (;;) {
        char nm[8]; VAL v;
        skipsp(&p);
        if (!isnamestart((unsigned char)*p)) break;
        readName(&p, nm);
        skipsp(&p);
        if (*p!='=') { fatal("syntax error"); return; }
        p++;
        v = parseConcatExpr(&p);
        storeInto(nm, 0, 0,0,0, v);
        skipsp(&p);
        if (*p==',') { p++; continue; }
        break;
    }
    skipsp(&p);
    if (*p==')') p++;
}

static void doDimension(char *p)
{
    for (;;) {
        char nm[8]; int dims[3]; int los[3]; int ns;
        skipsp(&p);
        if (!isnamestart((unsigned char)*p)) break;
        readName(&p, nm);
        ns = parseDimList(&p, dims, los, 3);
        declareArray(nm, ns, dims, los, 0, 0);
        skipsp(&p);
        if (*p==',') { p++; continue; }
        break;
    }
}

static void doTypeDecl(char *p, int type)
{
    for (;;) {
        char nm[8];
        skipsp(&p);
        if (!isnamestart((unsigned char)*p)) break;
        readName(&p, nm);
        skipsp(&p);
        if (*p=='(') {
            int dims[3]; int los[3]; int ns;
            ns = parseDimList(&p, dims, los, 3);
            declareArray(nm, ns, dims, los, type, 1);
        } else {
            VAR *v = getvar(nm);
            v->type = type;
        }
        skipsp(&p);
        if (*p==',') { p++; continue; }
        break;
    }
}

/* CHARACTER [*len] name1[*len1][(dims)], name2[*len2][(dims)], ... */
static void doCharacterDecl(char *p)
{
    int deflen = 1;
    skipsp(&p);
    if (*p=='*') {
        char nb[16]; int n=0;
        p++; skipsp(&p);
        while (isdigit((unsigned char)*p) && n<15) nb[n++]=*p++;
        nb[n]=0; if (n>0) deflen = atoi(nb);
        skipsp(&p);
        if (*p==',') p++;
    }
    for (;;) {
        char nm[8]; int len = deflen; VAR *v;
        int ns=0, dims[3], los[3];
        skipsp(&p);
        if (!isnamestart((unsigned char)*p)) break;
        readName(&p, nm);
        skipsp(&p);
        /* *len and (dims) may appear in either order */
        for (;;) {
            if (*p=='*') {
                char nb[16]; int n=0;
                p++; skipsp(&p);
                while (isdigit((unsigned char)*p) && n<15) nb[n++]=*p++;
                nb[n]=0; if (n>0) len = atoi(nb);
                skipsp(&p);
                continue;
            }
            if (*p=='(' && ns==0) {
                ns = parseDimList(&p, dims, los, 3);
                skipsp(&p);
                continue;
            }
            break;
        }
        if (len < 1) len = 1;
        v = getvar(nm);
        if (v->type == 2 && (v->cval || v->carr)) {
            /* Already-allocated CHARACTER storage -- either an earlier
               declaration in this scope, or a formal parameter aliased
               to the caller's actual argument. Just note the declared
               length/dims for bookkeeping; never reallocate, or a
               by-reference CHARACTER parameter would lose its data. */
            v->clen = len;
            if (ns > 0) { int k; v->ndims = ns; for (k=0;k<ns;k++) { v->dims[k] = dims[k]; v->lo[k] = los[k]; } }
        } else {
            v->type = 2;
            v->clen = len;
            if (ns > 0) {
                long size = 1; int k;
                for (k=0;k<ns;k++) size *= dims[k];
                v->isArray = 1; v->ndims = ns;
                for (k=0;k<ns;k++) { v->dims[k] = dims[k]; v->lo[k] = los[k]; }
                v->size = size;
                v->carr = (char*)malloc((size_t)(size*len));
                memset(v->carr, ' ', (size_t)(size*len));
            } else {
                v->isArray = 0;
                v->cval = (char*)malloc((size_t)len + 1);
                memset(v->cval, ' ', (size_t)len);
                v->cval[len] = 0;
            }
        }
        skipsp(&p);
        if (*p==',') { p++; continue; }
        break;
    }
}

/* parse an assignment target "NAME" or "NAME(sub,sub)" then '=' then expr, and store */
/* Looks ahead (no side effects) to see whether "p" (positioned at
   '(') begins a statement-function definition, i.e. a parenthesised
   list of bare dummy-argument names followed by '='. Returns the
   arg names, count, and a pointer to the body expression text. */
static int looksLikeStmtFuncDef(char *p, char names[MAXSFARGS][8], int *nargs, char **bodyStart)
{
    char *q = p;
    int n = 0;
    if (*q != '(') return 0;
    q++;
    for (;;) {
        char nm[8]; char *look;
        skipsp(&q);
        if (!isnamestart((unsigned char)*q)) return 0;
        look = q;
        readName(&look, nm);
        skipsp(&look);
        if (*look != ',' && *look != ')') return 0; /* subscript expr, not bare name */
        if (n < MAXSFARGS) { strcpy(names[n], nm); n++; }
        q = look;
        if (*q==',') { q++; continue; }
        if (*q==')') { q++; break; }
        return 0;
    }
    skipsp(&q);
    if (*q != '=') return 0;
    q++;
    *nargs = n;
    *bodyStart = q;
    return 1;
}

static void doAssignOrCall(char *p)
{
    char nm[8]; char *save;
    skipsp(&p);
    save = p;
    readName(&p, nm);
    skipsp(&p);
    if (*p=='(') {
        if (!findvarAliased(nm)) {
            char names[MAXSFARGS][8]; int nargs; char *bodyStart;
            if (looksLikeStmtFuncDef(p, names, &nargs, &bodyStart)) {
                STMTFUNC *sf;
                int i;
                if (nstmtfuncs >= MAXSTMTFUNC) { fatal("out of memory"); return; }
                sf = &stmtfuncs[nstmtfuncs++];
                strncpy(sf->name, nm, 7); sf->name[7]=0;
                sf->nargs = nargs;
                for (i=0;i<nargs;i++) { strncpy(sf->args[i], names[i], 7); sf->args[i][7]=0; getvar(names[i]); }
                strncpy(sf->body, bodyStart, LINELEN-1); sf->body[LINELEN-1]=0;
                return;
            }
        }
        {
        VAL subs[3]; int ns; long i1=1,i2=1,i3=1; VAL rhs;
        getvar(nm);
        ns = parseArgList(&p, subs, 3);
        if (ns>=1) i1=VI(subs[0]);
        if (ns>=2) i2=VI(subs[1]);
        if (ns>=3) i3=VI(subs[2]);
        skipsp(&p);
        if (*p!='=') { fatal("syntax error"); return; }
        p++;
        rhs = parseConcatExpr(&p);
        storeInto(nm, 1, i1,i2,i3, rhs);
        return;
        }
    }
    skipsp(&p);
    if (*p=='=') {
        VAL rhs;
        p++;
        rhs = parseConcatExpr(&p);
        storeInto(nm, 0, 0,0,0, rhs);
        return;
    }
    fatal("syntax error");
    (void)save;
}

static void doWriteOrPrint(char *p, int isWrite)
{
    long lab; int listDirected=0; const char *fmt; char *lp;
    inlineFmtSet = 0;
    if (isWrite) {
        lab = parseUnitFmt(&p, &listDirected);
    } else {
        /* PRINT label, list  or  PRINT *, list  or  PRINT 'fmt', list */
        skipsp(&p);
        if (*p=='*') { listDirected=1; p++; }
        else if (*p=='\'') {
            p = (char*)scanQuotedString(p, inlineFmtBuf, sizeof(inlineFmtBuf));
            inlineFmtSet = 1;
            lab = 0;
        }
        else {
            char nb[16]; int n=0;
            while (isdigit((unsigned char)*p) && n<15) nb[n++]=*p++;
            nb[n]=0; lab=atol(nb);
        }
        skipsp(&p);
        if (*p==',') p++;
    }
    lp = p;
    noutbuf = 0;
    skipsp(&lp);
    if (*lp) parseOutList(&lp, 0);
    if (listDirected) { emitOp(isWrite?"WRITE":"PRNT","*"); listDirectedPrint(noutbuf); return; }
    if (inlineFmtSet) {
        fmt = inlineFmtBuf;
        emitOpQuoted(isWrite?"WRITE":"PRNT", fmt);
    } else {
        fmt = findFormat(lab);
        if (!fmt) { fatal("undefined line number"); return; }
        { char nb[16]; sprintf(nb,"%ld",lab); emitOp(isWrite?"WRITE":"PRNT", nb); }
    }
    applyFormat(fmt, noutbuf);
}

/* Parses one decimal number (optional sign, optional fractional
   part, no exponent notation) from a char** cursor using nothing but
   integer accumulation and float MULTIPLICATION -- deliberately
   never division, since a floating-point division elsewhere (the
   CPU-time calculation) was the one confirmed cause of a real hang
   on real hardware this project has been tested on. Used by READ
   instead of scanf()'s own numeric parsing, which independently
   hung on that same hardware even after ruling out stdio buffering
   as the cause -- rather than keep guessing which specific part of
   Turbo C's scanf is at fault, this removes it from the picture. */
static VAL parseSignedNumber(char **p)
{
    int neg = 0;
    long ipart = 0, frac = 0;
    int fracDigits = 0, isReal = 0;
    skipsp(p);
    if (**p=='+') { (*p)++; }
    else if (**p=='-') { neg=1; (*p)++; }
    while (isdigit((unsigned char)**p)) { ipart = ipart*10 + (**p-'0'); (*p)++; }
    if (**p=='.') {
        isReal = 1;
        (*p)++;
        while (isdigit((unsigned char)**p)) { frac = frac*10 + (**p-'0'); fracDigits++; (*p)++; }
    }
    if (isReal) {
        float f = (float)ipart;
        float scale = 1.0f;
        int i;
        for (i=0;i<fracDigits;i++) scale *= 0.1f; /* multiplication only, never division */
        f += (float)frac * scale;
        if (neg) f = -f;
        return mkreal(f);
    }
    return mkint(neg ? -ipart : ipart);
}

static void doRead(char *p)
{
    long lab; int listDirected=0; char *lp;
    char inbuf[LINELEN]; char *ip;
    lab = parseUnitFmt(&p, &listDirected);
    (void)lab;
    emitOp("READ", NULL);
    printf("? ");
    fflush(stdout);
    lp = p;
    skipsp(&lp);
    /* Read the whole line with fgets, already proven to work on
       stdin on real hardware (it's how every REPL command gets
       read), then parse each value from the buffered line with
       parseSignedNumber above -- no scanf call anywhere in this path. */
    if (!fgets(inbuf, sizeof(inbuf), stdin)) return;
    ip = inbuf;
    for (;;) {
        char nm[8]; VAR *var; VAL v;
        skipsp(&lp);
        if (!isnamestart((unsigned char)*lp)) break;
        readName(&lp, nm);
        skipsp(&lp);
        var = getvar(nm);
        skipsp(&ip);
        if (*ip==',') ip++, skipsp(&ip); /* tolerate a comma-separated input line too */
        v = parseSignedNumber(&ip);
        if (*lp=='(') {
            VAL subs[3]; int ns; long i1=1,i2=1,i3=1; long idx;
            ns = parseArgList(&lp, subs, 3);
            if (ns>=1) i1=VI(subs[0]);
            if (ns>=2) i2=VI(subs[1]);
            if (ns>=3) i3=VI(subs[2]);
            idx = arrIndex(var,i1,i2,i3);
            if (var->type==0) var->iarr[idx]=VI(v);
            else var->rarr[idx]=VF(v);
        } else {
            if (var->type==0) var->ival=VI(v);
            else var->rval=VF(v);
        }
        skipsp(&lp);
        if (*lp==',') { lp++; continue; }
        break;
    }
}

static void doDo(char *p)
{
    char *q = p; long lab; char nm[8]; VAL e1,e2,e3; DOFRAME *fr;
    char nb[16]; int n=0;
    skipsp(&q);
    while (isdigit((unsigned char)*q) && n<15) nb[n++]=*q++;
    nb[n]=0; lab = atol(nb);
    skipsp(&q);
    readName(&q, nm);
    skipsp(&q);
    if (*q!='=') { fatal("syntax error"); return; }
    q++;
    e1 = parseExprTop(&q);
    skipsp(&q);
    if (*q!=',') { fatal("syntax error"); return; }
    q++;
    e2 = parseExprTop(&q);
    skipsp(&q);
    e3 = mkint(1);
    if (*q==',') { q++; e3 = parseExprTop(&q); }
    if (ndostk >= MAXDO) { fatal("out of memory"); return; }
    fr = &dostk[ndostk++];
    strncpy(fr->name, nm, 7); fr->name[7]=0;
    {
        VAR *v = getvar(nm);
        fr->isReal = v->type;
        if (v->type==0) v->ival = VI(e1); else v->rval = VF(e1);
    }
    fr->stop = VF(e2); fr->step = VF(e3);
    fr->istop = VI(e2); fr->istep = VI(e3);
    if (fr->istep==0) fr->istep=1;
    if (fr->step==0) fr->step=1;
    fr->endLabel = lab;
    fr->bodyIdx = pc + 1;
    { char nb[16]; sprintf(nb,"%s,%ld",nm,lab); emitOp("FOR", nb); }
}

static void doGoto(char *p)
{
    char nb[16]; int n=0; long lab; int idx;
    skipsp(&p);
    while (isdigit((unsigned char)*p) && n<15) nb[n++]=*p++;
    nb[n]=0; lab=atol(nb);
    idx = findLabel(lab);
    if (idx<0) { fatal("undefined line number"); return; }
    emitOp("JMP", nb);
    pc = idx - 1; /* -1 because caller does pc++ afterward via loop structure; we set directly */
}

static void doCall(char *p)
{
    char nm[8]; SUBPROG *sp; CALLFRAME *cf;
    skipsp(&p);
    readName(&p, nm);
    skipsp(&p);
    sp = findSubprog(nm);
    if (!sp) { fatal("undefined subroutine"); return; }
    if (ncallstack >= MAXCALLDEPTH) { fatal("out of memory"); return; }
    cf = &callstack[ncallstack];
    cf->nbinds = 0;
    if (*p=='(') {
        p++;
        skipsp(&p);
        if (*p != ')') {
            for (;;) {
                VAR *actual = NULL;
                skipsp(&p);
                if (isnamestart((unsigned char)*p)) {
                    char anm[8]; char *look = p;
                    readName(&look, anm);
                    skipsp(&look);
                    if (*look != '(') {
                        /* bare variable name (scalar or whole array): true alias */
                        p = look;
                        actual = getvar(anm);
                    }
                }
                if (!actual) {
                    /* anything else (literal, expression, subscripted element):
                       evaluate once into a scratch var -- writes inside the
                       callee won't propagate back, since there's no single
                       element to alias in our VAR model */
                    VAL v = parseConcatExpr(&p);
                    int slot = cf->nbinds < MAXSUBARGS ? cf->nbinds : 0;
                    VAR *tmp = &scratchvars[ncallstack][slot];
                    tmp->isArray = 0; tmp->type = v.isStr ? 2 : (v.isReal ? 1 : 0);
                    if (v.isStr) {
                        size_t vlen = strlen(v.s);
                        if (vlen > 127) vlen = 127;
                        memset((char*)scratchbuf[ncallstack][slot], ' ', 127);
                        scratchbuf[ncallstack][slot][127] = 0;
                        memcpy((char*)scratchbuf[ncallstack][slot], v.s, vlen);
                        tmp->clen = (int)vlen;
                        tmp->cval = (char*)scratchbuf[ncallstack][slot];
                    } else { tmp->ival = VI(v); tmp->rval = VF(v); }
                    actual = tmp;
                }
                if (cf->nbinds < MAXSUBARGS && cf->nbinds < sp->nargs) {
                    strncpy(cf->binds[cf->nbinds].formalName, sp->args[cf->nbinds], 7);
                    cf->binds[cf->nbinds].formalName[7] = 0;
                    cf->binds[cf->nbinds].actual = actual;
                    cf->nbinds++;
                }
                skipsp(&p);
                if (*p==',') { p++; continue; }
                break;
            }
        }
        skipsp(&p);
        if (*p==')') p++;
    }
    cf->returnIdx = pc;
    ncallstack++;
    emitOp("CALL", nm);
    pc = sp->bodyIdx - 1;
}

static void doReturn(void)
{
    if (ncallstack == 0) { fatal("return without call"); return; }
    ncallstack--;
    emitOp("RET", NULL);
    pc = callstack[ncallstack].returnIdx;
}

static void execOneStatement(char *stmt); /* fwd */

static void doIf(char *p)
{
    char *q = p; VAL cond; char *afterparen;
    skipsp(&q);
    if (*q!='(') { fatal("syntax error"); return; }
    q++;
    cond = evalLogical(&q);
    skipsp(&q);
    if (*q!=')') { fatal("syntax error"); return; }
    q++;
    afterparen = q;
    skipsp(&afterparen);
    /* arithmetic IF: rest is  L1,L2,L3 (digits and commas only) */
    {
        char *t = afterparen; int arithmetic = 1;
        if (!isdigit((unsigned char)*t)) arithmetic = 0;
        while (arithmetic && *t) {
            if (isdigit((unsigned char)*t) || *t==',' || *t==' ') t++;
            else { arithmetic = 0; }
        }
        if (arithmetic) {
            long labs[3]; int nl=0; char *t2=afterparen;
            while (*t2 && nl<3) {
                char nb[16]; int n=0;
                skipsp(&t2);
                while (isdigit((unsigned char)*t2) && n<15) nb[n++]=*t2++;
                nb[n]=0; if (n>0) labs[nl++]=atol(nb);
                skipsp(&t2);
                if (*t2==',') t2++;
            }
            if (nl==3) {
                float fv = VF(cond);
                long target;
                if (fv<0) target=labs[0]; else if (fv==0) target=labs[1]; else target=labs[2];
                {
                    int idx = findLabel(target);
                    if (idx<0) { fatal("undefined line number"); return; }
                    { char nb[48]; sprintf(nb,"%ld,%ld,%ld",labs[0],labs[1],labs[2]); emitOp("AIF", nb); }
                    pc = idx - 1;
                }
                return;
            }
        }
    }
    /* logical IF: execute remainder as a statement if cond true */
    emitOp("IF", VI(cond)?"T":"F");
    if (VI(cond)) {
        execOneStatement(afterparen);
    }
}

/* ------------------------------------------------------------------
   statement dispatcher
   ------------------------------------------------------------------ */
static void execOneStatement(char *stmt)
{
    char *p = stmt;
    skipsp(&p);
    if (*p==0) return;
    if (kwis(p,"DIMENSION")) { doDimension(p+9); return; }
    if (kwis(p,"PARAMETER")) { doParameter(p+9); return; }
    if (kwis(p,"DATA"))      { doData(p+4); return; }
    if (kwis(p,"INTEGER"))   { doTypeDecl(p+7, 0); return; }
    if (kwis(p,"REAL"))      { doTypeDecl(p+4, 1); return; }
    if (kwis(p,"CHARACTER")) { doCharacterDecl(p+9); return; }
    if (kwis(p,"CONTINUE"))  { return; }
    if (kwis(p,"STOP"))      { emitOp("HALT",NULL); stopFlag = 1; running = 0; return; }
    if (kwis(p,"END")) {
        if (ncallstack > 0) { doReturn(); return; } /* implicit RETURN at end of a subprogram */
        emitOp("HALT",NULL); stopFlag = 1; running = 0; return;
    }
    if (kwis(p,"CALL"))      { doCall(p+4); return; }
    if (kwis(p,"RETURN"))    { doReturn(); return; }
    if (kwis(p,"SUBROUTINE")) { return; /* pre-scanned; body is only ever entered via CALL */ }
    if (kwis(p,"GOTO"))      { doGoto(p+4); return; }
    if (kwis(p,"GO")) {
        char *q = p+2; skipsp(&q);
        if (kwis(q,"TO")) { doGoto(q+2); return; }
    }
    if (kwis(p,"IF"))        { doIf(p+2); return; }
    if (kwis(p,"DO"))        { doDo(p+2); return; }
    if (kwis(p,"WRITE"))     { doWriteOrPrint(p+5, 1); return; }
    if (kwis(p,"PRINT"))     { doWriteOrPrint(p+5, 0); return; }
    if (kwis(p,"READ"))      { doRead(p+4); return; }
    if (kwis(p,"FORMAT"))    { return; /* pre-scanned, no-op at exec time */ }
    if (kwis(p,"PROGRAM"))   { return; }
    doAssignOrCall(p);
}

/* ------------------------------------------------------------------
   program line storage / labels
   ------------------------------------------------------------------ */
static int findLabel(long lab)
{
    int i;
    for (i=0;i<nlines;i++) if (prog[i].used && prog[i].label==lab) return i;
    return -1;
}

static void prescanFormats(void)
{
    int i;
    nfmts = 0;
    for (i=0;i<nlines;i++) {
        char *p = (char*)prog[i].text;
        skipsp(&p);
        if (kwis(p,"FORMAT") && prog[i].used) {
            if (nfmts < MAXFORMATS) {
                fmts[nfmts].label = prog[i].label;
                strncpy((char*)fmts[nfmts].text, p+6, LINELEN-1);
                fmts[nfmts].text[LINELEN-1]=0;
                nfmts++;
            }
        }
    }
}

/* Registers every SUBROUTINE block found in the program: its name,
   formal parameter names, and the line-index range of its body, so
   CALL can find it regardless of textual order (defined before or
   after the point of call) and normal top-to-bottom execution can
   skip clean over the whole block rather than falling into it. */
static void prescanSubprograms(void)
{
    int i;
    nsubs = 0;
    for (i=0;i<nlines;i++) {
        char *p = (char*)prog[i].text;
        if (!prog[i].used) continue;
        skipsp(&p);
        if (kwis(p,"SUBROUTINE")) {
            SUBPROG *sp; char *q = p+10; char nm[8]; int j;
            if (nsubs >= MAXSUBS) { fatal("out of memory"); return; }
            skipsp(&q);
            readName(&q, nm);
            sp = &subs[nsubs++];
            strncpy(sp->name, nm, 7); sp->name[7]=0;
            sp->nargs = 0;
            skipsp(&q);
            if (*q=='(') {
                q++;
                skipsp(&q);
                while (*q && *q != ')') {
                    char anm[8];
                    skipsp(&q);
                    readName(&q, anm);
                    if (sp->nargs < MAXSUBARGS) {
                        strncpy(sp->args[sp->nargs], anm, 7);
                        sp->args[sp->nargs][7]=0;
                        sp->nargs++;
                    }
                    skipsp(&q);
                    if (*q==',') { q++; continue; }
                    break;
                }
                if (*q==')') q++;
            }
            sp->defIdx = i;
            sp->bodyIdx = i+1;
            j = i+1;
            while (j<nlines) {
                char *e = (char*)prog[j].text; char *eq=e;
                if (prog[j].used) { skipsp(&eq); if (kwis(eq,"END")) break; }
                j++;
            }
            sp->endIdx = j;
        }
    }
}

/* execute a single loaded program line (with DO-closing logic) */
static void execIndex(int idx)
{
    long lab = prog[idx].label;
    listStatementHeader(lab, (const char*)prog[idx].text);
    execOneStatement((char*)prog[idx].text);
    if (!running) return; /* STOP/END */
    if (lab != 0) {
        while (ndostk>0 && dostk[ndostk-1].endLabel==lab) {
            DOFRAME *fr = &dostk[ndostk-1];
            VAR *v = getvar(fr->name);
            int cont;
            if (fr->isReal) { v->rval += fr->step; cont = (fr->step>0)? (v->rval <= fr->stop+1e-4f) : (v->rval >= fr->stop-1e-4f); }
            else { v->ival += fr->istep; cont = (fr->istep>0)? (v->ival <= fr->istop) : (v->ival >= fr->istop); }
            if (cont) { emitOp("LOOP", fr->name); pc = fr->bodyIdx - 1; return; }
            else { emitOp("ENDFOR", fr->name); ndostk--; }
        }
    }
}

static void runFrom(int startIdx)
{
    running = 1; stopFlag = 0; ndostk = 0; ncallstack = 0;
    pc = startIdx;
    pcAddr = 0;
    if (traceMode) printf(" LOC   OP     OPERAND            STMT  SOURCE\n");
    while (running && pc < nlines) {
        if (!prog[pc].used) { pc++; continue; }
        {
            SUBPROG *hit = findSubprogAtLine(pc);
            if (hit) { pc = hit->endIdx + 1; continue; }
        }
        execIndex(pc);
        if (!running) break;
        pc++;
    }
    running = 0;
}

/* ------------------------------------------------------------------
   line loading (labelled -> stored; unlabelled -> immediate exec)
   ------------------------------------------------------------------ */
static void storeLine(long lab, const char *text)
{
    int i;
    for (i=0;i<nlines;i++) {
        if (prog[i].used && prog[i].label==lab) {
            strncpy((char*)prog[i].text, text, LINELEN-1); prog[i].text[LINELEN-1]=0;
            return;
        }
    }
    if (nlines >= MAXLINES) { fatal("out of memory"); return; }
    /* insert in sorted order by label */
    {
        int pos = nlines;
        for (i=0;i<nlines;i++) if (prog[i].label > lab) { pos=i; break; }
        for (i=nlines; i>pos; i--) prog[i]=prog[i-1];
        prog[pos].label = lab;
        strncpy((char*)prog[pos].text, text, LINELEN-1); prog[pos].text[LINELEN-1]=0;
        prog[pos].used = 1;
        nlines++;
    }
}

/* Splits a raw input line into (label, statement text), preserving
   whatever indentation follows the label -- or the whole line, if
   there's no label -- so a listing can show the programmer's own
   original nesting style, the way a real compiler listing does
   (that indentation was never compiler-generated; it's just the
   source file being echoed faithfully). Only the trailing newline/
   CR is stripped. Returns 0 if the line has no real content. */
static int splitLabelKeepIndent(char *buf, long *labOut, char **textOut)
{
    char *scan = buf;
    size_t L = strlen(buf);
    while (L>0 && (buf[L-1]=='\n' || buf[L-1]=='\r' || buf[L-1]==' ')) buf[--L]=0;
    skipsp(&scan);
    if (isdigit((unsigned char)*scan)) {
        char nb[16]; int n=0;
        while (isdigit((unsigned char)*scan) && n<15) nb[n++]=*scan++;
        nb[n]=0; *labOut = atol(nb);
        *textOut = scan;
    } else {
        *labOut = 0;
        *textOut = buf;
    }
    { char *chk = *textOut; skipsp(&chk); if (*chk==0) return 0; }
    return 1;
}

static int loadLine(const char *raw)
{
    char buf[LINELEN]; char *text; long lab;
    strncpy(buf, raw, LINELEN-1); buf[LINELEN-1]=0;
    if (!splitLabelKeepIndent(buf, &lab, &text)) return 0; /* blank: no-op, no Ok */
    if (lab) {
        storeLine(lab, text);
        return 0; /* stored a program line: no Ok, like real MBASIC */
    }
    /* immediate execution */
    execOneStatement(text);
    return 1;
}

/* append a line to the program in strict file order (no sort, no
   dedup by label) -- used when loading a batch source file, since
   many lines share label 0 and execution order must match the file. */
static void appendLineDirect(long lab, const char *text)
{
    if (nlines >= MAXLINES) { fatal("out of memory"); return; }
    prog[nlines].label = lab;
    strncpy((char*)prog[nlines].text, text, LINELEN-1); prog[nlines].text[LINELEN-1]=0;
    prog[nlines].used = 1;
    nlines++;
}

static void appendLine(const char *raw)
{
    char buf[LINELEN]; char *text; long lab;
    strncpy(buf, raw, LINELEN-1); buf[LINELEN-1]=0;
    if (!splitLabelKeepIndent(buf, &lab, &text)) return; /* skip blank lines */
    appendLineDirect(lab, text);
}

static int loadFile(const char *fn)
{
    FILE *f = fopen(fn, "r");
    char line[LINELEN];
    long reads = 0;
    if (!f) return 0;
    setvbuf(f, NULL, _IONBF, 0); /* bypass Turbo C's internal stdio buffering --
        on at least one real machine this project has been tested against, a
        LOAD would hang indefinitely in fgets() even though DOS's own
        unbuffered TYPE command read the identical file instantly; if that's
        a buffering-layer mismatch with the board/BIOS rather than a DOS- or
        disk-level problem, going unbuffered here should sidestep it */
    while (fgets(line, sizeof(line), f)) {
        char *q = line; /* peek pointer only -- never passed onward, so line's original indentation survives */
        /* Defensive bound, not a real limit on file size: a handful
           of environments (some DOSBox local-directory mounts among
           them) have been seen not to signal end-of-file cleanly on
           every read, which would otherwise spin this loop forever
           with no visible symptom at all. A read-attempt count this
           generous is well past anything a real source file should
           ever approach, so it only ever trips in that failure mode. */
        reads++;
        if (reads > 20000L) {
            fclose(f);
            return 0;
        }
        if (line[0]=='C' || line[0]=='c') continue; /* comment: C must be column 1, else it collides with CHARACTER */
        skipsp(&q);
        if (*q=='*' || *q=='\0') continue; /* '*' comment marker (never a valid statement start), or blank line */
        if (*q=='+' || *q=='>' || *q=='$' || *q=='&') {
            /* continuation of the previously appended line -- real
               fixed-form FORTRAN allows any non-blank, non-'0'
               character here; different shops used different ones
               ('+' and '>' both appear in real-world listings this
               project has been checked against), so several common
               ones are accepted (not digits, though, since those
               mean a label in this free-form reading, and not '*',
               which is already claimed as a comment marker). */
            if (nlines > 0) {
                char *cont = q + 1;
                size_t curlen = strlen((char*)prog[nlines-1].text);
                size_t L;
                skipsp(&cont);
                L = strlen(cont);
                while (L>0 && (cont[L-1]=='\n' || cont[L-1]=='\r' || cont[L-1]==' ')) cont[--L]=0;
                if (curlen + 1 < (size_t)(LINELEN-1)) {
                    strcat((char*)prog[nlines-1].text, " ");
                    strncat((char*)prog[nlines-1].text, cont, (LINELEN-2-curlen));
                }
            }
            continue;
        }
        appendLine(line);
    }
    fclose(f);
    prescanFormats();
    prescanSubprograms();
    return 1;
}

static void listProgram(void)
{
    int i;
    for (i=0;i<nlines;i++) if (prog[i].used) {
        if (prog[i].label) printf("%5ld %s\n", prog[i].label, prog[i].text);
        else                printf("      %s\n", prog[i].text);
    }
}

/* returns a display string for a variable's type, e.g. "INTEGER",
   "REAL", or "CHARACTER*10" */
static const char *typeName(int type, int clen)
{
    static char buf[24];
    if (type==0) return "INTEGER";
    if (type==1) return "REAL";
    sprintf(buf, "CHARACTER*%d", clen);
    return buf;
}

/* Case-insensitive whole-word search: does NAME (already uppercase,
   and already truncated to the interpreter's 7-char symbol limit)
   appear as a complete identifier anywhere in TEXT? An identifier
   longer than 7 characters in the source is truncated to 7 before
   comparing, matching how readName() actually stores it -- so
   "GREETING" in source text is correctly found under the truncated
   symbol name "GREETIN". */
static int containsWord(const char *text, const char *name)
{
    size_t nlen = strlen(name);
    const char *p = text;
    while (*p) {
        if (isnamestart((unsigned char)*p)) {
            const char *start = p;
            size_t len = 0;
            while (isnamechar((unsigned char)*p)) { p++; len++; }
            {
                size_t effLen = len > 7 ? 7 : len;
                if (effLen == nlen) {
                    size_t i; int match=1;
                    for (i=0;i<nlen;i++) if (toupper((unsigned char)start[i]) != (unsigned char)name[i]) { match=0; break; }
                    if (match) return 1;
                }
            }
        } else p++;
    }
    return 0;
}

/* Classifies how NAME is referenced on a given source line, using
   the same style of single-letter codes classic cross-reference
   listings used (s-Specified d-DO Index =-Assigned u-Used i-Input
   o-Output). This is a static, line-by-line heuristic over the
   source text -- not a runtime trace -- so a variable used inside a
   loop body gets exactly one entry per source line, the way a real
   compiler's cross-reference does, not once per iteration. Returns
   0 if NAME doesn't appear on this line at all. */
static char lineRefCode(int lineIdx, const char *name)
{
    char *raw = (char*)prog[lineIdx].text;
    char *q = raw;
    if (!containsWord(raw, name)) return 0;
    skipsp(&q);
    if (kwis(q,"DO")) {
        char *r = q+2; char nm[8]; char *look;
        skipsp(&r);
        while (isdigit((unsigned char)*r)) r++;
        skipsp(&r);
        look = r;
        readName(&look, nm);
        if (!strcmp(nm,name)) return 'd';
        return 'u';
    }
    if (kwis(q,"DIMENSION") || kwis(q,"INTEGER") || kwis(q,"REAL") || kwis(q,"CHARACTER")) return 's';
    if (kwis(q,"DATA")) return '/';
    if (kwis(q,"WRITE") || kwis(q,"PRINT")) return 'o';
    if (kwis(q,"READ")) return 'i';
    if (kwis(q,"FORMAT")) return 0; /* literal text, not a variable reference */
    /* assignment target? NAME [ (subscript) ] = ... */
    if (isnamestart((unsigned char)*q)) {
        char nm[8]; char *look = q;
        readName(&look, nm);
        skipsp(&look);
        if (*look=='(') {
            int depth=0;
            do { if (*look=='(') depth++; else if (*look==')') depth--; look++; } while (*look && depth>0);
            skipsp(&look);
        }
        if (*look=='=' && look[1]!='=' && !strcmp(nm,name)) return '=';
    }
    return 'u';
}

/* Classifies how LAB is referenced on a given source line (g-GOTO
   d-DO Label @-FORMAT Statement f-FORMAT Usage i-Arithmetic IF
   s-Specified/defining line). Returns 0 if not referenced here. */
static char lineLabelRefCode(int lineIdx, long lab)
{
    char *raw = (char*)prog[lineIdx].text;
    char *q = raw;
    skipsp(&q);
    if (prog[lineIdx].label == lab) {
        if (kwis(q,"FORMAT")) return '@';
        return 's';
    }
    if (kwis(q,"GOTO")) {
        char *r=q+4; skipsp(&r);
        if (atol(r)==lab) return 'g';
    }
    if (kwis(q,"GO")) {
        char *r=q+2; skipsp(&r);
        if (kwis(r,"TO")) { r+=2; skipsp(&r); if (atol(r)==lab) return 'g'; }
    }
    if (kwis(q,"DO")) {
        char *r=q+2; skipsp(&r);
        if (atol(r)==lab) return 'd';
    }
    if (kwis(q,"WRITE") || kwis(q,"PRINT") || kwis(q,"READ")) {
        char *r = strchr(q,',');
        if (r) { char *s=r+1; skipsp(&s); if (atol(s)==lab) return 'f'; }
    }
    if (kwis(q,"IF")) {
        /* arithmetic IF (expr) l1,l2,l3 -- just check if lab appears
           as one of a trailing comma-separated digit list */
        char *r = strrchr(q,')');
        if (r) {
            r++;
            while (*r) {
                char *s=r; long v;
                while (*s==' ') s++;
                v = atol(s);
                if (v==lab && isdigit((unsigned char)*s)) return 'i';
                r = strchr(r,','); if (!r) break; r++;
            }
        }
    }
    return 0;
}

/* Finds the PROGRAM statement's name, if any, for the listing
   header banner; falls back to "MAIN" like most compilers do when
   no PROGRAM statement is present. */
static void findProgramName(char *out, size_t outsz)
{
    int i;
    for (i=0;i<nlines;i++) {
        char *p = (char*)prog[i].text;
        skipsp(&p);
        if (kwis(p,"PROGRAM")) {
            char nm[8]; char *q = p+7;
            skipsp(&q);
            readName(&q, nm);
            strncpy(out, nm, outsz-1); out[outsz-1]=0;
            return;
        }
    }
    strncpy(out, "MAIN", outsz-1); out[outsz-1]=0;
}

static void printDateTimeLine(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[32];
    if (tm) strftime(buf, sizeof(buf), "%m/%d/%y  %H:%M:%S", tm);
    else strcpy(buf, "unknown date/time");
    printf("%s", buf);
}

/* Static, compile-style source listing: a one-time sequentially
   numbered dump of the whole program as loaded, with a page-header
   banner -- the part of a real compiler listing that happens once,
   before anything runs, as opposed to the runtime object trace
   (which is unique to being an interpreter and has no equivalent in
   a real compile listing). */
#define XREF_SEPLINE "________________________________________________________________________________\n"
#define DASHLINE      "--------------------------------------------------\n"

static void printSourceListing(void)
{
    char pname[8]; int i, nlab=0;
    findProgramName(pname, sizeof(pname));

    printf(DASHLINE);
    printf("iFOR-IV Interactive FORTRAN IV   Rev. 0.88\n");
    printf("Lawless Cybernetics Inc.    All Rights Reserved\n");
    printf("Compiled ");
    printDateTimeLine();
    printf("\n");
    printf(DASHLINE);
    printf("  Compiling: PROGRAM %s\n", pname);
    printf("  Programmer: Mickey W. Lawless\n");

    for (i=0;i<nlines;i++) {
        if (!prog[i].used) continue;
        if (prog[i].label) { printf("%6d  %-4ld  %s\n", i+1, prog[i].label, prog[i].text); nlab++; }
        else                printf("%6d        %s\n", i+1, prog[i].text);
    }

    printf("\n");
    printf(DASHLINE);
    printf("Compilation of %s\n", pname);
    printf("        Lines: %d\n", nlines);
    printf("        Labels: %d\n", nlab);
    printf(DASHLINE);
}

/* Post-execution Symbol/Label Cross-Reference Listing, in the style
   of the classic compiler listings this project is modeled on: for
   every variable, array, and statement function, every source line
   where it's referenced with a single-letter code for how; then the
   same for every statement label. Built by a static scan over the
   loaded source text (not the runtime trace), so a variable touched
   inside a loop gets exactly one entry per source line, matching
   what a real compiler's cross-reference shows -- not once per
   iteration. */
static void printRefList(int lineIdx0, char (*codeFn)(int,long), long key,
                          char (*nameCodeFn)(int,const char*), const char *name)
{
    int i, n=0;
    (void)lineIdx0;
    for (i=0;i<nlines;i++) {
        if (!prog[i].used) continue;
        {
            char code = nameCodeFn ? nameCodeFn(i, name) : codeFn(i, key);
            if (code) {
                printf("%5d%c ", i+1, code);
                n++;
                if (n % 8 == 0) printf("\n            ");
            }
        }
    }
    if (n==0) printf("(unreferenced)");
    putchar('\n');
}

static void printCrossReference(void)
{
    int i, nlabels, nscalar=0, narray=0;

    printf("\n");
    printf("Symbol Cross-Reference Listing - Reference types:\n\n");
    printf(" s-Specified; /-DATA; d-DO Index; =-Assigned; u-Used; i-Input; o-Output\n");
    printf(XREF_SEPLINE);
    printf("Name     Type            Class      Line<type>\n");
    printf(XREF_SEPLINE);
    printf("\n");
    for (i=0;i<nvars;i++) {
        VAR *v = &vars[i];
        if (v->isArray) narray++; else nscalar++;
        printf("%-8s %-15s %-10s ", v->name, typeName(v->type, v->clen), v->isArray?"ARRAY":"SCALAR");
        printRefList(0, NULL, 0, lineRefCode, v->name);
    }
    for (i=0;i<nstmtfuncs;i++) {
        STMTFUNC *sf = &stmtfuncs[i];
        printf("%-8s %-15s %-10s ", sf->name,
               implicitType(sf->name)==0 ? "INTEGER" : "REAL", "STMT FUNC");
        printRefList(0, NULL, 0, lineRefCode, sf->name);
    }

    printf("\nLabel Cross-Reference Listing - Reference types:\n\n");
    printf(" s-Specified; d-DO Label; @-FORMAT Statement; f-FORMAT Usage; g-GOTO; i-Arith IF\n");
    printf(XREF_SEPLINE);
    printf("Label    Line<type>\n");
    printf(XREF_SEPLINE);
    printf("\n");
    nlabels = 0;
    for (i=0;i<nlines;i++) {
        if (prog[i].used && prog[i].label) {
            int j, dup=0;
            for (j=0;j<i;j++) if (prog[j].used && prog[j].label==prog[i].label) dup=1;
            if (dup) continue; /* already listed this label */
            nlabels++;
            printf("%-8ld ", prog[i].label);
            printRefList(0, lineLabelRefCode, prog[i].label, NULL, NULL);
        }
    }
    if (nlabels==0) printf("(none)\n");

    printf("\nNo diagnostics.  %d symbol%s (%d variable%s, %d array%s, %d statement function%s), %d label%s.\n",
           nvars + nstmtfuncs, (nvars+nstmtfuncs)==1?"":"s",
           nscalar, nscalar==1?"":"s", narray, narray==1?"":"s",
           nstmtfuncs, nstmtfuncs==1?"":"s",
           nlabels, nlabels==1?"":"s");
}

static void newProgram(void)
{
    nlines = 0; nfmts = 0; nvars = 0; ndostk = 0;
}

/* ------------------------------------------------------------------
   ed-style edit submenu: lines are auto-numbered as you type them,
   so you don't have to supply a label for every statement -- but a
   line that DOES start with a number is stored under that explicit
   label instead (handy for DO/GOTO/IF targets), and auto-numbering
   then resumes ten past it. Leave the submenu either by typing RUN
   or EX (which also executes the listing), or by entering Ctrl-Z
   (ASCII 26, a la CP/M/DOS ed-alikes) or plain EOF, which returns
   to the command prompt without running anything.
   ------------------------------------------------------------------ */
/* ------------------------------------------------------------------
   ed-style edit submenu. Most FORTRAN statements don't carry a
   label at all, so lines are NOT auto-numbered by default here --
   only FORMAT and CONTINUE are, since a label is what makes them
   useful in the first place (an unlabeled FORMAT can never be
   referenced; an unlabeled CONTINUE serves no purpose). Any other
   line gets a label only if you type one yourself, exactly the way
   you'd mark a future GOTO/DO/arithmetic-IF target. Leave the
   submenu either by typing RUN or EX (which also executes the
   listing), or by entering Ctrl-Z (ASCII 26, a la CP/M/DOS
   ed-alikes) or plain EOF, which returns to the command prompt
   without running anything.
   ------------------------------------------------------------------ */
/* ------------------------------------------------------------------
   ed-style edit submenu. Most FORTRAN statements don't carry a
   label at all, so lines are NOT auto-numbered here. A label is
   stored only when you type one yourself -- exactly the way you'd
   mark a FORMAT statement, a CONTINUE used as a loop target, or any
   other GOTO/DO/arithmetic-IF target, matching whatever number you
   already used to refer to it elsewhere (auto-guessing a number
   here would be actively wrong: a WRITE(6,100) needs a FORMAT
   labeled exactly 100, not whatever a counter happens to produce
   next). Leave the submenu either by typing RUN or EX (which also
   executes the listing), or by entering Ctrl-Z (ASCII 26, a la
   CP/M/DOS ed-alikes) or plain EOF, which returns to the command
   prompt without running anything.
   ------------------------------------------------------------------ */
static void doEditSubmenu(void)
{
    char raw[LINELEN];
    printf("EDIT MODE -- LINES NEED NO LABEL UNLESS YOU WANT ONE.\n");
    printf("TYPE A LEADING NUMBER TO LABEL A LINE (FORMAT, A CONTINUE\n");
    printf("USED AS A TARGET, OR ANY GOTO/DO/IF TARGET), MATCHING\n");
    printf("WHATEVER NUMBER YOU REFERENCE IT BY ELSEWHERE.\n");
    printf("RUN OR EX TO EXECUTE AND LEAVE; CTRL-Z OR EOF TO ABORT\n\n");
    fflush(stdout);
    for (;;) {
        char buf[LINELEN]; char *p; long lab; char *text;
        if (!fgets(raw, sizeof(raw), stdin)) { putchar('\n'); return; } /* EOF */
        strncpy(buf, raw, LINELEN-1); buf[LINELEN-1] = 0;
        if ((unsigned char)buf[0] == 26) return; /* Ctrl-Z as first byte of the line */
        p = buf;
        skipsp(&p); /* peek only, for keyword checks -- doesn't mutate buf */
        if ((unsigned char)*p == 26) return;
        if (kwis(p,"RUN") || kwis(p,"EX")) {
            runAndReport(0);
            return;
        }
        if (!splitLabelKeepIndent(buf, &lab, &text)) continue; /* blank line: ignore */
        if (!lab) {
            char *chk = text; skipsp(&chk);
            if (kwis(chk,"FORMAT")) { printf("?Format statement has no label -- it can never be referenced\n"); fflush(stdout); }
        }
        appendLineDirect(lab, text);
    }
}

/* ------------------------------------------------------------------
   main / REPL
   ------------------------------------------------------------------ */
static void printBanner(void)
{
    long workspace = (long)(sizeof(vars)+sizeof(prog)+sizeof(fmts)
                            +sizeof(outbuf)+sizeof(stmtfuncs)+sizeof(dostk));
    printf("iFOR-IV Rev.0.88\n");
    printf("Interactive ANSI FORTRAN IV Compiler/Interpreter\n");
    printf("Copyright [1966] 2025,2026 (C) Lawless Cybernetics Inc.\n");
    printf("%ld BYTES WORKSPACE\n\n", workspace);
    printf("READY\n\n");
    fflush(stdout);
}

static void runAndReport(int startIdx)
{
    clock_t t0, t1;
    long ticks, wholeSec, centis;
    prescanFormats();
    prescanSubprograms();
    if (listMode) printSourceListing();
    hadError = 0;
    t0 = clock();
    runFrom(startIdx);
    t1 = clock();
    /* Integer arithmetic only, deliberately -- this is otherwise the
       only floating-point operation touched by running a program
       that itself never uses REAL/F/E, so it's the first FP
       instruction Turbo C's runtime would execute on such a run,
       and a real suspect for hangs tied to FP emulation/coprocessor
       setup on real hardware with no coprocessor. */
    ticks = (long)(t1 - t0);
    wholeSec = ticks / (long)CLOCKS_PER_SEC;
    centis = ((ticks % (long)CLOCKS_PER_SEC) * 100L) / (long)CLOCKS_PER_SEC;
    if (hadError)
        printf("\nEXECUTION TERMINATED ABNORMALLY.  CPU TIME: %ld.%02ld SEC\n", wholeSec, centis);
    else
        printf("\nEXECUTION TERMINATED.  CPU TIME: %ld.%02ld SEC\n", wholeSec, centis);
    if (listMode) printCrossReference();
}

int main(int argc, char **argv)
{
    char *fname = NULL;
    int i;
    setvbuf(stdin, NULL, _IONBF, 0); /* same reasoning as the fopen()'d-file
        fix in loadFile() -- scanf() (used by READ) goes through this same
        buffered stdio layer, and a READ hanging after input is typed on
        real hardware where LOAD was already hanging identically before
        that fix points at the same underlying buffering mismatch, just on
        stdin instead of a file handle */
    printBanner();
    for (i=1;i<argc;i++) {
        if (!strcmp(argv[i],"-l") || !strcmp(argv[i],"-list") || !strcmp(argv[i],"-listing")) listMode = 1;
        else if (!strcmp(argv[i],"-t") || !strcmp(argv[i],"-trace")) traceMode = 1;
        else fname = argv[i];
    }
    if (fname) {
        if (!loadFile(fname)) { fprintf(stderr,"?File not found\n"); return 1; }
        runAndReport(0);
        return 0;
    }
    {
        char line[LINELEN];
        printf("Ok\n");
        fflush(stdout);
        for (;;) {
            int showOk = 1;
            if (!fgets(line, sizeof(line), stdin)) break;
            {
                char *p = line; skipsp(&p);
                if (kwis(p,"BYE") || kwis(p,"QUIT")) { printf("\nLOGGED OFF\n"); break; }
                if (kwis(p,"NEW")) { newProgram(); }
                else if (kwis(p,"LIST")) { listProgram(); }
                else if (kwis(p,"EDIT") || kwis(p,"ED") || kwis(p,"AUTO")) { doEditSubmenu(); }
                else if (kwis(p,"LISTING")) {
                    char *q=p+7; skipsp(&q);
                    if (kwis(q,"ON")) listMode = 1;
                    else if (kwis(q,"OFF")) listMode = 0;
                    /* bare LISTING: just report status, don't change it */
                    printf("LISTING IS %s\n", listMode ? "ON" : "OFF");
                }
                else if (kwis(p,"TRACE")) {
                    char *q=p+5; skipsp(&q);
                    if (kwis(q,"ON")) traceMode = 1;
                    else if (kwis(q,"OFF")) traceMode = 0;
                    printf("TRACE IS %s\n", traceMode ? "ON" : "OFF");
                }
                else if (kwis(p,"RUN")) { runAndReport(0); }
                else if (kwis(p,"LOAD")) {
                    char *q=p+4, fn[128]; skipsp(&q);
                    if (*q=='"') q++;
                    { int n=0; while (*q && *q!='"' && *q!='\n' && n<127) fn[n++]=*q++; fn[n]=0; }
                    newProgram();
                    if (!loadFile(fn)) printf("?File not found\n");
                    else printf("FILE LOADED\n");
                }
                else if (kwis(p,"SAVE")) {
                    char *q=p+4, fn[128]; FILE *f;
                    skipsp(&q);
                    if (*q=='"') q++;
                    { int n=0; while (*q && *q!='"' && *q!='\n' && n<127) fn[n++]=*q++; fn[n]=0; }
                    f = fopen(fn,"w");
                    if (f) { int j; for (j=0;j<nlines;j++) if (prog[j].used) { if (prog[j].label) fprintf(f,"%5ld %s\n", prog[j].label, prog[j].text); else fprintf(f,"      %s\n", prog[j].text); } fclose(f); printf("FILE SAVED\n"); }
                    else printf("?File not found\n");
                }
                else {
                    showOk = loadLine(line); /* 1 if immediate exec, 0 if a line was just stored */
                }
            }
            if (showOk) { printf("Ok\n"); fflush(stdout); }
        }
    }
    return 0;
}
