//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//===========================================================================//

// scriplib.c

#include "tier1/strtools.h"
#include "tier2/tier2.h"
#include "cmdlib.h"
#include "scriplib.h"
#include <stdlib.h>
#if defined( _X360 )
#include "xbox\xbox_win32stubs.h"
#endif
#if defined(POSIX)
#include "../../filesystem/linux_support.h"
#include <sys/stat.h>
#endif
/*
=============================================================================

						PARSING STUFF

=============================================================================
*/

typedef struct
{
	char	filename[1024];
	char    *buffer,*script_p,*end_p;
	int     line;

	char	macrobuffer[4096];
	char	*macroparam[64];
	char	*macrovalue[64];
	int		nummacroparams;

} script_t;

#define	MAX_INCLUDES	16
script_t	scriptstack[MAX_INCLUDES];
script_t	*script = NULL;
int			scriptline;

char    token[MAXTOKEN];
qboolean endofscript;
qboolean tokenready;                     // only true if UnGetToken was just called

typedef struct 
{
	char *param;
	char *value;
	char *param_lcase;
} variable_t;

CUtlVector<variable_t> g_definevariable;

CUtlVector<CUtlString> g_includeDirs;

/*
Callback stuff
*/

void DefaultScriptLoadedCallback( char const *pFilenameLoaded, char const *pIncludedFromFileName, int nIncludeLineNumber )
{
	NULL;
}

SCRIPT_LOADED_CALLBACK g_pfnCallback = DefaultScriptLoadedCallback;

SCRIPT_LOADED_CALLBACK SetScriptLoadedCallback( SCRIPT_LOADED_CALLBACK pfnNewScriptLoadedCallback )
{
	SCRIPT_LOADED_CALLBACK pfnCallback = g_pfnCallback;
	g_pfnCallback = pfnNewScriptLoadedCallback;
	return pfnCallback;
}

/*
==============
AddScriptToStack
==============
*/
void AddScriptToStack (char *filename, ScriptPathMode_t pathMode = SCRIPT_USE_ABSOLUTE_PATH)
{
	int            size;

	script++;
	if (script == &scriptstack[MAX_INCLUDES])
		Error ("script file exceeded MAX_INCLUDES");
	
	if ( pathMode == SCRIPT_USE_RELATIVE_PATH )
		Q_strncpy( script->filename, filename, sizeof( script->filename ) );
	else
		Q_strncpy (script->filename, ExpandPath (filename), sizeof( script->filename ) );

	size = LoadFile (script->filename, (void **)&script->buffer);

	// printf ("entering %s\n", script->filename);
	if ( g_pfnCallback )
	{
		if ( script == scriptstack + 1 )
			g_pfnCallback( script->filename, NULL, 0 );
		else
			g_pfnCallback( script->filename, script[-1].filename, script[-1].line );
	}

	script->line = 1;

	script->script_p = script->buffer;
	script->end_p = script->buffer + size;
}


/*
==============
LoadScriptFile
==============
*/
void LoadScriptFile (char *filename, ScriptPathMode_t pathMode)
{
	script = scriptstack;
	AddScriptToStack (filename, pathMode);

	endofscript = false;
	tokenready = false;
}


/*
==============
==============
*/

#define MAX_MACROS 128
script_t	*macrolist[MAX_MACROS];
int nummacros;

void DefineMacro( char *macroname )
{
	for ( int i = 0; i < nummacros; i++ )
	{
		if ( strcmpi( macrolist[i]->filename, macroname ) == 0 )
		{
			Error( "\"$definemacro %s\" is already defined.\n", macroname );
		}
	}

	script_t	*pmacro = (script_t *)malloc( sizeof( script_t ) );

	strcpy( pmacro->filename, macroname );
	pmacro->line = script->line;
	pmacro->nummacroparams = 0;

	char *mp = pmacro->macrobuffer;
	char *cp = script->script_p;

	while (TokenAvailable( ))
	{
		GetToken( false );

		if (token[0] == '\\' && token[1] == '\\')
		{
			break;
		}
		cp = script->script_p;

		pmacro->macroparam[pmacro->nummacroparams++] = mp;

		strcpy( mp, token );
		mp += strlen( token ) + 1;

		if (mp >= pmacro->macrobuffer + sizeof( pmacro->macrobuffer ))
			Error("Macro buffer overflow\n");
	}
	// roll back script_p to previous valid location
	script->script_p = cp;

	// check if this line uses \\ continuation (old style) or $endmacro terminator (new style)
	char *scan = cp;
	bool has_backslash = false;
	while (*scan && *scan != '\n')
	{
		if (*scan == '\\' && *(scan+1) == '\\')
		{
			has_backslash = true;
			break;
		}
		scan++;
	}

	if (has_backslash)
	{
		// old \\ continuation style: each body line ends with \\, last line does not
		while (*cp && *cp != '\n')
		{
			if (*cp == '\\' && *(cp+1) == '\\')
			{
				// blank out \\ and everything after it on this line, then continue to next
				while (*cp && *cp != '\n')
				{
					*cp = ' ';
					cp++;
				}
				if (*cp) cp++;
			}
			else
			{
				cp++;
			}
		}

		int size = (int)(cp - script->script_p);
		pmacro->buffer = (char *)malloc( size + 1 );
		memcpy( pmacro->buffer, script->script_p, size );
		pmacro->buffer[size] = '\0';
		pmacro->end_p = &pmacro->buffer[size];
	}
	else
	{
		// $endmacro terminator style: body follows on subsequent lines, terminated by $endmacro
		// skip the rest of the definition line (past the params)
		while (*cp && *cp != '\n') cp++;
		if (*cp) cp++;

		char *body_start = cp;
		char *body_end = cp;
		char *after_endmacro = cp;

		while (*cp)
		{
			// skip leading whitespace to check for $endmacro token
			char *check = cp;
			while (*check == ' ' || *check == '\t') check++;

			if (_strnicmp( check, "$endmacro", 9 ) == 0)
			{
				char c = *(check + 9);
				if (c == '\0' || c == '\n' || c == '\r' || c == ' ' || c == '\t')
				{
					body_end = cp;
					while (*check && *check != '\n') check++;
					if (*check) check++;
					after_endmacro = check;
					break;
				}
			}

			while (*cp && *cp != '\n') cp++;
			if (*cp) cp++;
			body_end = cp;
			after_endmacro = cp;
		}

		int size = (int)(body_end - body_start);
		pmacro->buffer = (char *)malloc( size + 1 );
		if (size > 0)
			memcpy( pmacro->buffer, body_start, size );
		pmacro->buffer[size] = '\0';
		pmacro->end_p = &pmacro->buffer[size];
		cp = after_endmacro;
	}

	macrolist[nummacros++] = pmacro;
	if ( nummacros == MAX_MACROS )
		Error ("script file exceeded MAX_MACROS");

	script->script_p = cp;
}


void DefineVariable( char *variablename )
{
	variable_t v;

	v.param = strdup( variablename );

	GetToken( false );
	
	v.value = strdup( token );
	
	v.param_lcase = strlwr( strdup(v.param) );

	for ( int i=0; i<g_definevariable.Count(); i++ )
	{
		if ( !V_strcmp( g_definevariable[i].param_lcase, v.param_lcase ) )
		{
			Warning( "\"$definevariable %s\" is defined more than once (previous value \"%s\", new value \"%s\").\n", v.param_lcase, g_definevariable[i].value, v.value );
		}
	}

	g_definevariable.AddToTail( v );
}

void RedefineVariable( char *variablename )
{
	variable_t v;

	v.param = strdup( variablename );

	GetToken( false );
	
	v.value = strdup( token );

	int nIdx = -1;
	for ( int i=0; i<g_definevariable.Count(); i++ )
	{
		if ( !V_strcmp( g_definevariable[i].param, v.param ) )
		{
			nIdx = i;
			break;
		}
	}

	if ( nIdx >= 0 )
	{
		g_definevariable[nIdx] = v;
	}
	else
	{
		Error("Cannot redefine undefined variable \"%s\". Use $definevariable instead.\n", v.param );
	}
}


void SetVariable( char *variablename )
{
	variable_t v;

	v.param = strdup( variablename );

	GetToken( false );

	v.value = strdup( token );

	v.param_lcase = strlwr( strdup(v.param) );

	for ( int i=0; i<g_definevariable.Count(); i++ )
	{
		if ( !V_strcmp( g_definevariable[i].param, v.param ) )
		{
			free( g_definevariable[i].param );
			free( g_definevariable[i].value );
			free( g_definevariable[i].param_lcase );
			g_definevariable[i] = v;
			return;
		}
	}

	g_definevariable.AddToTail( v );
}


void DefineVariableDirect(const char* name, const char* value) {
    for (int i = 0; i < g_definevariable.Count(); i++) {
        if (!Q_strcmp(g_definevariable[i].param, name)) {
            free(g_definevariable[i].param);
            free(g_definevariable[i].value);
            free(g_definevariable[i].param_lcase);
            g_definevariable[i].param       = strdup(name);
            g_definevariable[i].value       = strdup(value);
            g_definevariable[i].param_lcase = strlwr(strdup(name));
            return;
        }
    }
    variable_t v;
    v.param       = strdup(name);
    v.value       = strdup(value);
    v.param_lcase = strlwr(strdup(name));
    g_definevariable.AddToTail(v);
}

void AddIncludeDir(const char* dir) {
    g_includeDirs.AddToTail(dir);
}

const char* LookupVariableValue(const char* name) {
    for (int i = 0; i < g_definevariable.Count(); i++) {
        if (!Q_strcmp(g_definevariable[i].param, name))
            return g_definevariable[i].value;
    }
    char nameLc[256];
    V_strncpy(nameLc, name, sizeof(nameLc));
    strlwr(nameLc);
    for (int i = 0; i < g_definevariable.Count(); i++) {
        if (!Q_strcmp(g_definevariable[i].param_lcase, nameLc))
            return g_definevariable[i].value;
    }
    return nullptr;
}

bool IsVariableDefined(const char* name) {
    return LookupVariableValue(name) != nullptr;
}

/*
==============
Conditional processing ($if/$elif/$else, $switch/$case/$default)
==============
*/

static int s_conditionalDepth = 0;
// Tunable nesting/branch limits live in scriplib.h (MAX_CONDITIONAL_*).

typedef struct {
    char* data;
    int   len;
    int   cap;
} ScripBuf;

static void ScripBuf_Init(ScripBuf* s) { s->data = nullptr; s->len = 0; s->cap = 0; }

static void ScripBuf_PushChar(ScripBuf* s, char c) {
    if (s->len + 2 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->data = (char*)realloc(s->data, s->cap);
        if (!s->data) Error("ScripBuf: out of memory\n");
    }
    s->data[s->len++] = c;
    s->data[s->len] = '\0';
}

static void ScripBuf_Free(ScripBuf* s) { free(s->data); ScripBuf_Init(s); }
static bool ScripBuf_Empty(const ScripBuf* s) { return s->len == 0; }

// Transfer ownership: dst gets src's buffer, src becomes empty
static void ScripBuf_Steal(ScripBuf* dst, ScripBuf* src) {
    ScripBuf_Free(dst);
    *dst = *src;
    ScripBuf_Init(src);
}

typedef struct {
    char toks[MAX_CONDITIONAL_TOKENS][MAXTOKEN];
    bool isLiteral[MAX_CONDITIONAL_TOKENS];
    int  count;
} CondToks;

static void CondToks_Init(CondToks* c) { c->count = 0; }

static void CondToks_Push(CondToks* c, const char* tok, bool isLiteral) {
    if (c->count >= MAX_CONDITIONAL_TOKENS)
        Error("$if: too many tokens in condition (max %d)\n", MAX_CONDITIONAL_TOKENS);
    c->isLiteral[c->count] = isLiteral;
    V_strncpy(c->toks[c->count++], tok, MAXTOKEN);
}

// Collect raw chars from current script_p ('{' already consumed) up to matching '}'.
// Closing '}' is consumed but not stored. Updates script->script_p and line counters.
static void CollectBlockContentRaw(ScripBuf* out) {
    const char* p = script->script_p;
    int depth = 1;
    bool inQuote = false;
    while (p < script->end_p && depth > 0) {
        char c = *p;
        if (inQuote) {
            ScripBuf_PushChar(out, c);
            if (c == '"') inQuote = false;
            p++;
        } else if (c == '"') {
            inQuote = true;
            ScripBuf_PushChar(out, c);
            p++;
        } else if (c == '/' && (p + 1) < script->end_p && *(p + 1) == '/') {
            while (p < script->end_p && *p != '\n') ScripBuf_PushChar(out, *p++);

        } else if (c == '#' || c == ';') {
            while (p < script->end_p && *p != '\n') ScripBuf_PushChar(out, *p++);

        } else if (c == '/' && (p + 1) < script->end_p && *(p + 1) == '*') {
            ScripBuf_PushChar(out, *p++); ScripBuf_PushChar(out, *p++);
            while ((p + 1) < script->end_p && !(*p == '*' && *(p + 1) == '/')) {
                if (*p == '\n') { script->line++; scriptline = script->line; }
                ScripBuf_PushChar(out, *p++);
            }
            if ((p + 1) < script->end_p) { ScripBuf_PushChar(out, *p++); ScripBuf_PushChar(out, *p++); }
        } else if (c == '{') {
            depth++;
            ScripBuf_PushChar(out, c); p++;
        } else if (c == '}') {
            depth--;
            if (depth > 0) ScripBuf_PushChar(out, c);
            p++;
        } else {
            if (c == '\n') { script->line++; scriptline = script->line; }
            ScripBuf_PushChar(out, c); p++;
        }
    }
    script->script_p = (char*)p;
}

// Push raw source text onto the script stack for transparent execution.
// EndOfScript will free the buffer and pop the stack automatically.
static void PushConditionalBlock(ScripBuf* content, int startLine) {
    if (ScripBuf_Empty(content)) return;
    if (script == nullptr) script = scriptstack;
    script_t* parent = script;
    script++;
    if (script == &scriptstack[MAX_INCLUDES])
        Error("script file exceeded MAX_INCLUDES");
    script->nummacroparams = parent->nummacroparams;
    for (int i = 0; i < parent->nummacroparams; i++) {
        script->macroparam[i] = parent->macroparam[i];
        script->macrovalue[i] = parent->macrovalue[i];
    }
    int sz = content->len;
    char* buf = (char*)malloc(sz + 2);
    if (!buf) Error("PushConditionalBlock: out of memory\n");
    memcpy(buf, content->data, sz);
    buf[sz]     = '\n';
    buf[sz + 1] = '\0';
    V_strcpy(script->filename, "conditional block");
    script->buffer   = buf;
    script->script_p = buf;
    script->end_p    = buf + sz + 1;
    script->line     = startLine;
    scriptline       = startLine;
    endofscript      = false;
    tokenready       = false;
}

// ---- Condition expression evaluator ----

static bool s_isTruthy(const char* s) {
    if (!s || !*s) return false;
    if (!strcmp(s, "0")) return false;
    if (!Q_stricmp(s, "false")) return false;
    return true;
}

static bool s_isNumericStr(const char* s) {
    if (!s || !*s) return false;
    if (*s == '-' || *s == '+') s++;
    if (!*s) return false;
    bool hasDig = false;
    while (*s >= '0' && *s <= '9') { s++; hasDig = true; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { s++; hasDig = true; } }
    return hasDig && (*s == '\0');
}

static bool s_compareValues(const char* lhs, const char* op, const char* rhs) {
    bool num = s_isNumericStr(lhs) && s_isNumericStr(rhs);
    double dL = num ? strtod(lhs, nullptr) : 0.0;
    double dR = num ? strtod(rhs, nullptr) : 0.0;
    int sc = strcmp(lhs, rhs);
    if (!strcmp(op, "==")) return num ? dL == dR : sc == 0;
    if (!strcmp(op, "=!")) return num ? dL != dR : sc != 0;
    if (!strcmp(op, ">"))  return num ? dL > dR  : sc > 0;
    if (!strcmp(op, "<"))  return num ? dL < dR  : sc < 0;
    if (!strcmp(op, ">=")) return num ? dL >= dR : sc >= 0;
    if (!strcmp(op, "<=")) return num ? dL <= dR : sc <= 0;
    return false;
}

// Membership comparison for in(): numeric when both sides parse as numbers,
// otherwise a case-insensitive string match (unlike ==, list membership ignores
// case so e.g. "Zoey" matches "zoey").
static bool s_inEquals(const char* val, const char* item) {
    if (s_isNumericStr(val) && s_isNumericStr(item))
        return strtod(val, nullptr) == strtod(item, nullptr);
    return Q_stricmp(val, item) == 0;
}

// in( <varname> [ <item> <item> ... ] )
// True when <varname>'s value equals any item in the bracketed list. Everything
// inside the brackets is literal (surrounding quotes are optional and stripped),
// so quoting individual items is purely cosmetic. A quoted item may contain
// spaces; bare items are whitespace-delimited. Comparison is numeric when both
// the value and the item parse as numbers, otherwise a case-insensitive string
// match.
static bool s_evalInExpr(const char* expr) {
    const char* open = strchr(expr, '(');
    if (!open) { Error("$if: malformed in(...) expression\n"); return false; }
    const char* p   = open + 1;
    const char* end = expr + strlen(expr);
    while (end > p && end[-1] != ')') end--;     // trim to the closing ')'
    if (end <= p) { Error("$if: malformed in(...) expression\n"); return false; }
    end--;                                       // content is now [p, end)

    // Variable name: raw name (no $...$), up to whitespace or '['.
    while (p < end && (unsigned char)*p <= ' ') p++;
    char vname[256]; int vl = 0;
    while (p < end && (unsigned char)*p > ' ' && *p != '[' && vl < 255) vname[vl++] = *p++;
    vname[vl] = '\0';
    if (vl == 0) { Error("$if: in() requires a variable name\n"); return false; }

    const char* val = LookupVariableValue(vname);
    if (!val) {
        Error("$if: undefined variable '%s' in in() (test with None(%s) first)\n", vname, vname);
        return false;
    }

    // Bracketed list.
    while (p < end && *p != '[') p++;
    if (p >= end) { Error("$if: in(%s ...) expected a '[ ... ]' list\n", vname); return false; }
    p++;                                         // past '['
    const char* listEnd = p;
    while (listEnd < end && *listEnd != ']') listEnd++;
    if (listEnd >= end) { Error("$if: in(%s ...) missing closing ']'\n", vname); return false; }

    const char* q = p;
    while (q < listEnd) {
        while (q < listEnd && (unsigned char)*q <= ' ') q++;
        if (q >= listEnd) break;
        char item[MAXTOKEN]; int il = 0;
        if (*q == '"') {
            q++;
            while (q < listEnd && *q != '"' && il < MAXTOKEN - 1) item[il++] = *q++;
            if (q < listEnd && *q == '"') q++;
        } else {
            while (q < listEnd && (unsigned char)*q > ' ' && il < MAXTOKEN - 1) item[il++] = *q++;
        }
        item[il] = '\0';
        if (s_inEquals(val, item)) return true;
    }
    return false;
}

typedef struct { const CondToks* ct; int pos; } CondEval;

static bool ce_atEnd(const CondEval* e) { return e->pos >= e->ct->count; }
static const char* ce_cur(const CondEval* e) { return e->ct->toks[e->pos]; }

static bool ce_evalOr(CondEval* e); // forward declaration for mutual recursion

static bool ce_evalAtom(CondEval* e, char* outBuf, int outBufSize) {
    if (ce_atEnd(e)) { Error("$if: unexpected end of condition\n"); outBuf[0] = '\0'; return false; }
    const char* t = ce_cur(e);

    if (!strcmp(t, "(")) {
        e->pos++;
        bool r = ce_evalOr(e);
        if (!ce_atEnd(e) && !strcmp(ce_cur(e), ")")) e->pos++;
        V_strncpy(outBuf, r ? "1" : "0", outBufSize);
        return r;
    }

    if (Q_strnicmp(t, "None(", 5) == 0 && strlen(t) > 5 && t[strlen(t)-1] == ')') {
        e->pos++;
        int vlen = (int)strlen(t) - 6;
        char vname[256]; vname[0] = '\0';
        if (vlen > 0 && vlen < 256) { memcpy(vname, t+5, vlen); vname[vlen] = '\0'; }
        const char* v = LookupVariableValue(vname);
        bool none = (!v || *v == '\0');
        V_strncpy(outBuf, none ? "1" : "0", outBufSize);
        return none;
    }

    if (Q_strnicmp(t, "Not(", 4) == 0 && strlen(t) > 4 && t[strlen(t)-1] == ')') {
        e->pos++;
        int ilen = (int)strlen(t) - 5;
        char inner[MAXTOKEN]; inner[0] = '\0';
        if (ilen > 0 && ilen < MAXTOKEN) { memcpy(inner, t+4, ilen); inner[ilen] = '\0'; }
        CondToks subct; CondToks_Init(&subct);
        V_strncpy(subct.toks[0], inner, MAXTOKEN); subct.isLiteral[0] = false; subct.count = 1;
        CondEval sub; sub.ct = &subct; sub.pos = 0;
        char tmp[MAXTOKEN]; tmp[0] = '\0';
        bool r = ce_evalAtom(&sub, tmp, sizeof(tmp));
        bool neg = !r;
        V_strncpy(outBuf, neg ? "1" : "0", outBufSize);
        return neg;
    }

    if (Q_strnicmp(t, "in(", 3) == 0 && strlen(t) > 3 && t[strlen(t)-1] == ')') {
        e->pos++;
        bool r = s_evalInExpr(t);
        V_strncpy(outBuf, r ? "1" : "0", outBufSize);
        return r;
    }

    bool literal = e->ct->isLiteral[e->pos];
    e->pos++;
    // A quoted/$var$-expanded operand, or anything numeric, is a literal value.
    // Any other bare operand is a variable reference and must be defined.
    if (!literal && !s_isNumericStr(t)) {
        const char* v = LookupVariableValue(t);
        if (!v) {
            Error("$if: undefined variable '%s' in condition "
                  "(quote string literals as \"%s\", or test with None(%s))\n", t, t, t);
            outBuf[0] = '\0';
            return false;
        }
        V_strncpy(outBuf, v, outBufSize);
        return s_isTruthy(v);
    }
    V_strncpy(outBuf, t, outBufSize);
    return s_isTruthy(t);
}

static bool ce_evalCmp(CondEval* e) {
    char lhs[MAXTOKEN]; lhs[0] = '\0';
    bool lhsBool = ce_evalAtom(e, lhs, sizeof(lhs));
    if (ce_atEnd(e)) return lhsBool;
    const char* op = ce_cur(e);
    if (!strcmp(op,"==") || !strcmp(op,"=!") || !strcmp(op,">") ||
        !strcmp(op,"<")  || !strcmp(op,">=") || !strcmp(op,"<=")) {
        e->pos++;
        char rhs[MAXTOKEN]; rhs[0] = '\0';
        ce_evalAtom(e, rhs, sizeof(rhs));
        return s_compareValues(lhs, op, rhs);
    }
    return lhsBool;
}

static bool ce_evalAnd(CondEval* e) {
    bool r = ce_evalCmp(e);
    while (!ce_atEnd(e) && !strcmp(ce_cur(e), "&&")) { e->pos++; bool rhs = ce_evalCmp(e); r = r && rhs; }
    return r;
}

static bool ce_evalOr(CondEval* e) {
    bool r = ce_evalAnd(e);
    while (!ce_atEnd(e) && !strcmp(ce_cur(e), "||")) { e->pos++; bool rhs = ce_evalAnd(e); r = r || rhs; }
    return r;
}

// Peek at the next non-whitespace, non-comment word without advancing script_p.
// Used by ProcessIfDirective to detect $elif/$else without consuming any input,
// so GetToken can safely error on standalone $elif/$else tokens.
static void s_peekRawWord(char* buf, int bufSize) {
    buf[0] = '\0';
    const char* p = script->script_p;
    while (p < script->end_p) {
        if ((unsigned char)*p <= ' ') { p++; continue; }
        if ((*p == '/' && p+1 < script->end_p && *(p+1) == '/') || *p == '#' || *p == ';') {
            while (p < script->end_p && *p != '\n') p++;
            continue;
        }
        if (*p == '/' && p+1 < script->end_p && *(p+1) == '*') {
            p += 2;
            while (p+1 < script->end_p && !(*p == '*' && *(p+1) == '/')) p++;
            if (p+1 < script->end_p) p += 2;
            continue;
        }
        break;
    }
    int len = 0;
    while (p < script->end_p && (unsigned char)*p > ' ' && *p != ';' && len < bufSize - 1)
        buf[len++] = *p++;
    buf[len] = '\0';
}

// Skip whitespace/comments/newlines from script_p and return the next
// significant character (0 at end of script), leaving script_p positioned at it
// so a following GetToken reads it directly.
static char s_peekSignificant() {
    const char* p   = script->script_p;
    const char* end = script->end_p;
    while (p < end) {
        char c = *p;
        if (c == '\n') { script->line++; scriptline = script->line; p++; continue; }
        if ((unsigned char)c <= ' ') { p++; continue; }
        if ((c == '/' && p + 1 < end && p[1] == '/') || c == '#' || c == ';') {
            while (p < end && *p != '\n') p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n') { script->line++; scriptline = script->line; }
                p++;
            }
            if (p + 1 < end) p += 2;
            continue;
        }
        break;
    }
    script->script_p = (char*)p;
    return (p < end) ? *p : 0;
}

// Consume the '{' that opens a directive body, or error with the given context.
static void s_expectBrace(const char* ctx) {
    if (s_peekSignificant() != '{') { Error("%s: expected '{'\n", ctx); return; }
    script->script_p++;
}

static bool s_ciPrefix(const char* p, const char* end, const char* pat) {
    for (; *pat; ++p, ++pat) {
        if (p >= end) return false;
        char a = *p, b = *pat;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

// An in( <var> [ ... ] ) membership test contains spaces (the bracketed list),
// so GetToken would shred it. When script_p sits on "in(" - or "not(in(" for a
// negated test - read the whole balanced-paren expression raw and push it as one
// token; the evaluator (s_evalInExpr, reached directly or via the Not() branch)
// parses it later. Returns false (consuming nothing) otherwise.
static bool s_tryReadInExpr(CondToks* toks) {
    const char* p   = script->script_p;
    const char* end = script->end_p;
    if (!(s_ciPrefix(p, end, "in(") || s_ciPrefix(p, end, "not(in(")))
        return false;

    char buf[MAXTOKEN]; int len = 0;
    int depth = 0;
    while (p < end) {
        char ch = *p;
        if (ch == '\n') { script->line++; scriptline = script->line; }
        if (ch == '(') depth++;
        else if (ch == ')') depth--;
        if (len < MAXTOKEN - 1) buf[len++] = ch;
        else { Error("$if: in(...) expression too long\n"); break; }
        p++;
        if (ch == ')' && depth == 0) break;              // consumed the matching ')'
    }
    buf[len] = '\0';
    if (depth != 0) { Error("$if: unterminated in(...) expression\n"); return true; }
    script->script_p = (char*)p;
    CondToks_Push(toks, buf, false);
    return true;
}

// Read a condition (the tokens up to the opening '{', which is consumed) into
// `toks`, tagging each token literal/non-literal. A token is literal when it
// begins with '"' (quoted string) or '$' (a $var$ expansion); bare words are
// resolved as variable references by the evaluator.
static void s_readCondition(CondToks* toks) {
    while (true) {
        char c = s_peekSignificant();
        if (c == 0)   { Error("$if: missing '{'\n"); return; }
        if (c == '{') { script->script_p++; return; }   // consume '{'
        if (s_tryReadInExpr(toks)) continue;            // in( var [ ... ] ) spans spaces
        bool isLit = (c == '"' || c == '$');
        GetToken(true);                                  // expands $var$, strips quotes
        CondToks_Push(toks, token, isLit);
    }
}

static bool s_evalConditionTokens(CondToks* toks) {
    if (toks->count == 0) { Error("$if: empty condition\n"); return false; }
    CondEval ev; ev.ct = toks; ev.pos = 0;
    bool result = ce_evalOr(&ev);
    if (ev.pos < ev.ct->count)
        Error("$if: unexpected token '%s' in condition\n", ev.ct->toks[ev.pos]);
    return result;
}

// Read one clause header of an $if chain (the condition tokens up to the opening
// '{', which is consumed) and return whether the condition is true.
static bool s_readIfClause() {
    CondToks toks; CondToks_Init(&toks);
    s_readCondition(&toks);
    return s_evalConditionTokens(&toks);
}

static void ProcessIfDirective() {
    if (++s_conditionalDepth > MAX_CONDITIONAL_NESTING) {
        Error("$if: maximum nesting depth (%d) exceeded\n", MAX_CONDITIONAL_NESTING);
        --s_conditionalDepth; return;
    }

    bool taken = false;
    ScripBuf chosen; ScripBuf_Init(&chosen);
    int chosenLine = scriptline;
    int elifCount = 0;

    {
        bool cond = s_readIfClause();
        int bline = scriptline;
        ScripBuf block; ScripBuf_Init(&block);
        CollectBlockContentRaw(&block);
        if (cond && !taken) { taken = true; chosenLine = bline; ScripBuf_Steal(&chosen, &block); }
        ScripBuf_Free(&block);
    }

    while (true) {
        // Peek without consuming - GetToken cannot be used here because
        // PushConditionalBlock clears tokenready. Peeking at raw chars means we
        // never advance script_p unless we actually find $elif/$else.
        char peeked[MAXTOKEN];
        s_peekRawWord(peeked, sizeof(peeked));
        if (!Q_stricmp(peeked, "$elif")) {
            GetToken(true); // consume the $elif token
            if (++elifCount > MAX_CONDITIONAL_ELIF)
                Error("$if: maximum $elif count (%d) exceeded\n", MAX_CONDITIONAL_ELIF);
            bool cond = s_readIfClause();
            int bline = scriptline;
            ScripBuf block; ScripBuf_Init(&block);
            CollectBlockContentRaw(&block);
            if (cond && !taken) { taken = true; chosenLine = bline; ScripBuf_Steal(&chosen, &block); }
            ScripBuf_Free(&block);
        } else if (!Q_stricmp(peeked, "$else")) {
            GetToken(true); // consume the $else token
            s_expectBrace("$else");
            int bline = scriptline;
            ScripBuf block; ScripBuf_Init(&block);
            CollectBlockContentRaw(&block);
            if (!taken) { taken = true; chosenLine = bline; ScripBuf_Steal(&chosen, &block); }
            ScripBuf_Free(&block);
            break;
        } else {
            break; // not $elif or $else, nothing was consumed - no restore needed
        }
    }

    --s_conditionalDepth;
    PushConditionalBlock(&chosen, chosenLine);
    ScripBuf_Free(&chosen);
}

// Handles $switch:  $switch var { $case v { } ... $default { } }
static void ProcessSwitchDirective() {
    if (++s_conditionalDepth > MAX_CONDITIONAL_NESTING) {
        Error("$switch: maximum nesting depth (%d) exceeded\n", MAX_CONDITIONAL_NESTING);
        --s_conditionalDepth; return;
    }

    if (!GetToken(false)) {
        Error("$switch: expected variable name\n"); --s_conditionalDepth; return;
    }
    const char* varValPtr = LookupVariableValue(token);
    if (!varValPtr) {
        Error("$switch: undefined variable '%s'\n", token); --s_conditionalDepth; return;
    }
    char switchVal[MAXTOKEN];
    V_strncpy(switchVal, varValPtr, sizeof(switchVal));

    s_expectBrace("$switch");

    bool caseTaken = false; bool hasDefault = false;
    int caseCount = 0;
    ScripBuf chosen; ScripBuf_Init(&chosen);
    int chosenLine = scriptline;

    while (true) {
        if (!GetToken(true) || endofscript) { Error("$switch: unexpected end of file\n"); break; }
        if (!stricmp("}", token)) break;

        if (!Q_stricmp("$case", token)) {
            if (++caseCount > MAX_CONDITIONAL_CASE)
                Error("$switch: maximum $case count (%d) exceeded\n", MAX_CONDITIONAL_CASE);
            if (!GetToken(false)) { Error("$case: expected value\n"); break; }
            char caseVal[MAXTOKEN]; V_strncpy(caseVal, token, sizeof(caseVal));
            s_expectBrace("$case");
            int bline = scriptline;
            ScripBuf block; ScripBuf_Init(&block);
            CollectBlockContentRaw(&block);
            if (!caseTaken && !strcmp(switchVal, caseVal)) {
                caseTaken = true; chosenLine = bline; ScripBuf_Steal(&chosen, &block);
            }
            ScripBuf_Free(&block);
        } else if (!Q_stricmp("$default", token)) {
            hasDefault = true;
            s_expectBrace("$default");
            int bline = scriptline;
            ScripBuf block; ScripBuf_Init(&block);
            CollectBlockContentRaw(&block);
            if (!caseTaken) { chosenLine = bline; ScripBuf_Steal(&chosen, &block); }
            ScripBuf_Free(&block);
        } else {
            Error("$switch: expected $case or $default, got '%s'\n", token); break;
        }
    }

    if (!hasDefault) Error("$switch: $default is required\n");
    --s_conditionalDepth;
    PushConditionalBlock(&chosen, chosenLine);
    ScripBuf_Free(&chosen);
}

/*
==============
==============
*/
bool AddMacroToStack( char *macroname )
{
	// lookup macro
	if (macroname[0] != '$')
		return false;

	int i;
	for (i = 0; i < nummacros; i++)
	{
		if (strcmpi( macrolist[i]->filename, &macroname[1] ) == 0)
		{
			break;
		}
	}
	if (i == nummacros)
		return false;

	script_t *pmacro = macrolist[i];

	// get tokens
	script_t	*pnext = script + 1;

	pnext++;
	if (pnext == &scriptstack[MAX_INCLUDES])
		Error ("script file exceeded MAX_INCLUDES");

	// get tokens
	char *cp = pnext->macrobuffer;

	pnext->nummacroparams = pmacro->nummacroparams;

	for (i = 0; i < pnext->nummacroparams; i++)
	{
		GetToken(false);

		strcpy( cp, token );
		pnext->macroparam[i] = pmacro->macroparam[i];
		pnext->macrovalue[i] = cp;

		cp += strlen( token ) + 1;

		if (cp >= pnext->macrobuffer + sizeof( pnext->macrobuffer ))
			Error("Macro buffer overflow\n");
	}

	script = pnext;
	strcpy( script->filename, pmacro->filename );

	int size = pmacro->end_p - pmacro->buffer;
	script->buffer = (char *)malloc( size + 1 );
	memcpy( script->buffer, pmacro->buffer, size );
	pmacro->buffer[size] = '\0';
	script->script_p = script->buffer;
	script->end_p = script->buffer + size;
	script->line = pmacro->line;

	return true;
}


bool ExpandSubMacroToken( char *&token_p )
{
	if ( *token_p == '^' )
	{
		token_p++;

		char * szPotentialVar = token_p;

		while ( *token_p != '^' )
		{
			token_p++;
		}

		*token_p = '\0';

		token_p = szPotentialVar;

		int index;
		for (index = 0; index < g_definevariable.Count(); index++)
		{
			if ( !Q_strcmp( g_definevariable[index].param, szPotentialVar ) )
			{
				strcpy( token, g_definevariable[index].value );
				return true;
			}
		}
	}
	return false;
}

bool ExpandMacroToken( char *&token_p )
{
	if ( script->nummacroparams && *script->script_p == '$' )
	{
		char *cp = script->script_p + 1;

		while ( *cp > 32 && *cp != '$' )
		{
			cp++;
		}

		// found a word with $'s on either end?
		if (*cp != '$')
			return false;

		// get token pointer
		char *tp = script->script_p + 1;
		int len = (cp - tp);
		*(tp + len) = '\0';

		// lookup macro parameter
		int index = 0;
		for (index = 0; index < script->nummacroparams; index++)
		{
			if (stricmp( script->macroparam[index], tp ) == 0)
				break;
		}
		if (index >= script->nummacroparams)
		{
			Error("unknown macro token \"%s\" in %s\n", tp, script->filename );
		}

		// paste token into 
		len = strlen( script->macrovalue[index] );
		strcpy( token_p, script->macrovalue[index] );
		token_p += len;
		
		script->script_p = cp + 1;

		if (script->script_p >= script->end_p)
			Error ("Macro expand overflow\n");

		if (token_p >= &token[MAXTOKEN])
			Error ("Token too large on line %i\n",scriptline);

		return true;
	}
	return false;
}



/*
==============
==============
*/
// FIXME: this should create a new script context so the individual tokens in the variable can be parsed
bool ExpandVariableToken( char *&token_p )
{
	if ( *script->script_p == '$' )
	{
		char *cp = script->script_p + 1;

		while ( *cp > 32 && *cp != '$' )
		{
			cp++;
		}

		// found a word with $'s on either end?
		if (*cp != '$')
			return false;

		// get token pointer
		char *tp = script->script_p + 1;
		int len = (cp - tp);
		*(tp + len) = '\0';

		// lookup macro parameter

		int index;
		for (index = 0; index < g_definevariable.Count(); index++)
		{
			// [wills] just strcmp here, this was doing nearest partial comparison before which could result in a false positive variable name match. Bad!
			if ( !Q_strcmp( g_definevariable[index].param, tp ) )
				break;
		}
	
		// if we can't find the variable, try again without case sensitivity, then complain loudly if we find anything
		if (index >= g_definevariable.Count() )
		{
			for (index = 0; index < g_definevariable.Count(); index++)
			{
				char *tp_lower = strlwr(strdup(tp));
				if ( !Q_strcmp( g_definevariable[index].param_lcase, tp_lower ) )
				{
					Warning( "Unknown variable token fell back to case-insensitive match ( found: \"%s\", matched it to: \"%s\" ) in %s\n", tp, g_definevariable[index].param, script->filename );
					break;
				}
			}
		}

		if (index >= g_definevariable.Count() )
		{
			Error("unknown variable token \"%s\" in %s\n", tp, script->filename );
		}

		// paste token into 
		len = strlen( g_definevariable[index].value );
		strcpy( token_p, g_definevariable[index].value );
		token_p += len;
		
		script->script_p = cp + 1;

		if (script->script_p >= script->end_p)
			Error ("Macro expand overflow\n");

		if (token_p >= &token[MAXTOKEN])
			Error ("Token too large on line %i\n",scriptline);

		return true;
	}
	return false;
}



/*
==============
ParseFromMemory
==============
*/
void ParseFromMemory (char *buffer, int size)
{
	script = scriptstack;
	script++;
	if (script == &scriptstack[MAX_INCLUDES])
		Error ("script file exceeded MAX_INCLUDES");
	strcpy (script->filename, "memory buffer" );

	script->buffer = buffer;
	script->line = 1;
	script->script_p = script->buffer;
	script->end_p = script->buffer + size;

	endofscript = false;
	tokenready = false;
}


//-----------------------------------------------------------------------------
// Used instead of ParseFromMemory to temporarily add a memory buffer
// to the script stack.  ParseFromMemory just blows away the stack.
//-----------------------------------------------------------------------------
// Saved unget state (tokenready + token buffer) for each pushed memory script,
// indexed by stack frame. Lets a pending UnGetToken survive reentrant memory-script
// parsing (e.g. flex-rule evaluation during a source load) instead of being clobbered.
static qboolean s_savedTokenReady[MAX_INCLUDES];
static char     s_savedToken[MAX_INCLUDES][MAXTOKEN];

void PushMemoryScript( char *pszBuffer, const int nSize )
{
	if ( script == NULL )
	{
		script = scriptstack;
	}
	script++;
	if ( script == &scriptstack[MAX_INCLUDES] )
	{
		Error ( "script file exceeded MAX_INCLUDES" );
	}
	strcpy (script->filename, "memory buffer" );

	script->buffer = pszBuffer;
	script->line = 1;
	script->script_p = script->buffer;
	script->end_p = script->buffer + nSize;

	// remember any pending UnGetToken belonging to the parent context so it can be
	// restored when this memory script is popped
	int frame = (int)(script - scriptstack);
	s_savedTokenReady[frame] = tokenready;
	Q_strncpy( s_savedToken[frame], token, sizeof( s_savedToken[frame] ) );

	endofscript = false;
	tokenready = false;
}


//-----------------------------------------------------------------------------
// Used after calling PushMemoryScript to clean up the memory buffer
// added to the script stack.  The normal end of script terminates
// all parsing at the end of a memory buffer even if there are more scripts
// remaining on the script stack
//-----------------------------------------------------------------------------
bool PopMemoryScript()
{
	if ( V_stricmp( script->filename, "memory buffer" ) )
		return false;

	if ( script == scriptstack )
	{
		endofscript = true;
		return false;
	}

	// restore any pending UnGetToken that belonged to the parent context
	int frame = (int)(script - scriptstack);
	tokenready = s_savedTokenReady[frame];
	if ( tokenready )
		Q_strncpy( token, s_savedToken[frame], MAXTOKEN );

	script--;
	scriptline = script->line;

	endofscript = false;

	return true;
}


/*
==============
UnGetToken

Signals that the current token was not used, and should be reported
for the next GetToken.  Note that

GetToken (true);
UnGetToken ();
GetToken (false);

could cross a line boundary.
==============
*/
void UnGetToken ()
{
	tokenready = true;
}


qboolean EndOfScript (qboolean crossline)
{
	if (!crossline)
		Error ("Line %i is incomplete\n",scriptline);

	if (!strcmp (script->filename, "memory buffer"))
	{
		endofscript = true;
		return false;
	}

	free (script->buffer);
	script->buffer = NULL;
	if (script == scriptstack+1)
	{
		endofscript = true;
		return false;
	}
	script--;
	scriptline = script->line;
	// printf ("returning to %s\n", script->filename);
	return GetToken (crossline);
}

void AttemptConditionalInclude( void )
{
	GetToken (false);

	char szSavedPath[MAX_PATH];
	V_strcpy( szSavedPath, token );

	// parse optional flags on the same line (any order, combinable)
	bool bIfFileExist   = false; // silently skip if file not found anywhere
	bool bNoFallbackDir = false; // ignore -includedir fallback dirs

	while ( TokenAvailable() )
	{
		GetToken (false);
		if ( !stricmp(token, "iffileexist") || !stricmp(token, "iffileexists") )
		{
			bIfFileExist = true;
		}
		else if ( !stricmp(token, "nofallbackdir") )
		{
			bNoFallbackDir = true;
		}
		else
		{
			Error ("Unknown $include parameter %s\n", token);
		}
	}

	// check primary location
	{
		char szPrimBuf[MAX_PATH];
		V_strncpy(szPrimBuf, szSavedPath, sizeof(szPrimBuf));
		const char* szExpanded = ExpandPath(szPrimBuf);
		FILE* fp = fopen(szExpanded, "r");
		if (fp)
		{
			fclose(fp);
			printf("Including: %s\n", szSavedPath);
			AddScriptToStack(szSavedPath);
			return;
		}
	}

	// try fallback include dirs in registration order (unless nofallbackdir)
	if ( !bNoFallbackDir )
	{
		// basename of the requested path (filename + extension, no directories)
		const char* szBaseName = V_UnqualifiedFileName( szSavedPath );

		for (int nDir = 0; nDir < g_includeDirs.Count(); nDir++)
		{
			char szCandidate[MAX_PATH];
			V_snprintf(szCandidate, sizeof(szCandidate), "%s/%s", g_includeDirs[nDir].String(), szSavedPath);
			Q_FixSlashes(szCandidate);
			const char* szExpanded = ExpandPath(szCandidate);
			FILE* fc = fopen(szExpanded, "r");
			if (fc)
			{
				fclose(fc);
				printf("Including: %s (from fallback dir: %s)\n", szSavedPath, g_includeDirs[nDir].String());
				AddScriptToStack(szCandidate);
				return;
			}

			// drop the relative hierarchy: <dir>/<basename>
			// (only meaningful when the requested path actually had subdirectories)
			if ( szBaseName && szBaseName[0] && Q_strcmp(szBaseName, szSavedPath) != 0 )
			{
				char szFlatCandidate[MAX_PATH];
				V_snprintf(szFlatCandidate, sizeof(szFlatCandidate), "%s/%s", g_includeDirs[nDir].String(), szBaseName);
				Q_FixSlashes(szFlatCandidate);
				const char* szFlatExpanded = ExpandPath(szFlatCandidate);
				FILE* ff = fopen(szFlatExpanded, "r");
				if (ff)
				{
					fclose(ff);
					printf("Including: %s (from fallback dir: %s, flattened)\n", szSavedPath, g_includeDirs[nDir].String());
					AddScriptToStack(szFlatCandidate);
					return;
				}
			}
		}
	}

	// file not found anywhere
	if ( bIfFileExist )
	{
		printf("SKIPPING (not found): %s\n", szSavedPath);
		return;
	}

	// preserve original behavior - AddScriptToStack will error on missing file
	printf("Including: %s\n", szSavedPath);
	AddScriptToStack(szSavedPath);
}

/*
==============
GetToken
==============
*/
qboolean GetToken (qboolean crossline)
{
	char    *token_p;

	if (tokenready)                         // is a token allready waiting?
	{
		tokenready = false;
		return true;
	}

	// printf("script_p %x (%x)\n", script->script_p, script->end_p ); fflush( stdout );

	if (script->script_p >= script->end_p)
	{
		return EndOfScript (crossline);
	}

	tokenready = false;

	// skip space, ctrl chars
skipspace:
	while (*script->script_p <= 32)
	{
		if (script->script_p >= script->end_p)
		{
			return EndOfScript (crossline);
		}
		if (*(script->script_p++) == '\n')
		{
			if (!crossline)
			{
				Error ("Line %i is incomplete\n",scriptline);
			}
			scriptline = ++script->line;
		}
	}

	if (script->script_p >= script->end_p)
	{
		return EndOfScript (crossline);
	}

	// strip single line comments
	if (*script->script_p == ';' || *script->script_p == '#' ||		 // semicolon and # is comment field
		(*script->script_p == '/' && *((script->script_p)+1) == '/')) // also make // a comment field
	{											
		if (!crossline)
			Error ("Line %i is incomplete\n",scriptline);
		while (*script->script_p++ != '\n')
		{
			if (script->script_p >= script->end_p)
			{
				return EndOfScript (crossline);
			}
		}
		scriptline = ++script->line;
		goto skipspace;
	}

	//  strip out matching /* */ comments
	if (*script->script_p == '/' && *((script->script_p)+1) == '*')
	{
		script->script_p += 2;
		while (*script->script_p != '*' || *((script->script_p)+1) != '/')
		{
			if (*script->script_p++ != '\n')
			{
				if (script->script_p >= script->end_p)
				{
					return EndOfScript (crossline);
				}

				scriptline = ++script->line;
			}
		}
		script->script_p += 2;
		goto skipspace;
	}

	// copy token to buffer
	token_p = token;

	if (*script->script_p == '"')
	{
		// quoted token - variable expansion applies inside quotes
		script->script_p++;
		while (*script->script_p != '"')
		{
			if (script->script_p == script->end_p)
				break;
			if (!ExpandVariableToken(token_p))
			{
				*token_p++ = *script->script_p++;
				if (script->script_p == script->end_p)
					break;
			}
			if (token_p == &token[MAXTOKEN])
				Error ("Token too large on line %i\n",scriptline);
		}
		script->script_p++;
	}
	else	// regular token
	while ( *script->script_p > 32 && *script->script_p != ';')
	{
		if ( !ExpandMacroToken( token_p ) )
		{
			if ( !ExpandVariableToken( token_p ) )
			{
				*token_p++ = *script->script_p++;
				if (script->script_p == script->end_p)
					break;
				if (token_p == &token[MAXTOKEN])
					Error ("Token too large on line %i\n",scriptline);

			}
		}
	}

	// add null to end of token
	*token_p = 0;

	// quick hack: check for submacro variables
	char *token_submacro = token;
	if ( ExpandSubMacroToken(token_submacro) )
	{
		return true;
	}

	// check for other commands
	if (!stricmp (token, "$include"))
	{
		AttemptConditionalInclude();
		return GetToken (crossline);
	}
	else if (!stricmp (token, "$definemacro"))
	{
		GetToken (false);
		DefineMacro(token);
		return GetToken (crossline);
	}
	else if (!stricmp (token, "$definevariable"))
	{
		GetToken (false);
		DefineVariable(token);
		return GetToken (crossline);
	}
	else if (!stricmp (token, "$redefinevariable"))
	{
		GetToken (false);
		RedefineVariable(token);
		return GetToken (crossline);
	}
	else if (!stricmp (token, "$setvariable"))
	{
		GetToken (false);
		SetVariable(token);
		return GetToken (crossline);
	}
	else if (!stricmp (token, "$if"))
	{
		ProcessIfDirective();
		return GetToken (crossline);
	}
	else if (!stricmp (token, "$switch"))
	{
		ProcessSwitchDirective();
		return GetToken (crossline);
	}
	else if (AddMacroToStack( token ))
	{
		return GetToken (crossline);
	}

	return true;
}


/*
==============
GetExprToken - use C mathematical operator parsing rules to split tokens instead of whitespace
==============
*/
qboolean GetExprToken (qboolean crossline)
{
	char    *token_p;

	if (tokenready)                         // is a token allready waiting?
	{
		tokenready = false;
		return true;
	}

	if (script->script_p >= script->end_p)
		return EndOfScript (crossline);

	tokenready = false;

//
// skip space
//
skipspace:
	while (*script->script_p <= 32)
	{
		if (script->script_p >= script->end_p)
			return EndOfScript (crossline);
		if (*script->script_p++ == '\n')
		{
			if (!crossline)
				Error ("Line %i is incomplete\n",scriptline);
			scriptline = ++script->line;
		}
	}

	if (script->script_p >= script->end_p)
		return EndOfScript (crossline);

	if (*script->script_p == ';' || *script->script_p == '#' ||		 // semicolon and # is comment field
		(*script->script_p == '/' && *((script->script_p)+1) == '/')) // also make // a comment field
	{											
		if (!crossline)
			Error ("Line %i is incomplete\n",scriptline);
		while (*script->script_p++ != '\n')
			if (script->script_p >= script->end_p)
				return EndOfScript (crossline);
		goto skipspace;
	}

//
// copy token
//
	token_p = token;

	if (*script->script_p == '"')
	{
		// quoted token
		script->script_p++;
		while (*script->script_p != '"')
		{
			*token_p++ = *script->script_p++;
			if (script->script_p == script->end_p)
				break;
			if (token_p == &token[MAXTOKEN])
				Error ("Token too large on line %i\n",scriptline);
		}
		script->script_p++;
	}
	else
	{
		if ( V_isalpha( *script->script_p ) || *script->script_p == '_' )
		{
			// regular token
			while ( V_isalnum( *script->script_p ) || *script->script_p == '_' )
			{
				*token_p++ = *script->script_p++;
				if (script->script_p == script->end_p)
					break;
				if (token_p == &token[MAXTOKEN])
					Error ("Token too large on line %i\n",scriptline);
			}
		}
		else if ( V_isdigit( *script->script_p ) || *script->script_p == '.' )
		{
			// regular token
			while ( V_isdigit( *script->script_p ) || *script->script_p == '.' )
			{
				*token_p++ = *script->script_p++;
				if (script->script_p == script->end_p)
					break;
				if (token_p == &token[MAXTOKEN])
					Error ("Token too large on line %i\n",scriptline);
			}
		}
		else if ( *script->script_p == '$' )
		{
			if ( !ExpandVariableToken( token_p ) )
			{
				// bare $ with no matching closing $ - treat as single char
				*token_p++ = *script->script_p++;
			}
		}
		else
		{
			// single char
			*token_p++ = *script->script_p++;
		}
	}

	*token_p = 0;

	if (!stricmp (token, "$include"))
	{
		AttemptConditionalInclude();
		return GetToken (crossline);
	}

	return true;
}


/*
==============
TokenAvailable

Returns true if there is another token on the line
==============
*/
qboolean TokenAvailable ()
{
	char    *search_p;

	if (tokenready)                         // is a token allready waiting?
	{
		return true;
	}

	search_p = script->script_p;

	if (search_p >= script->end_p)
		return false;

	while ( *search_p <= 32)
	{
		if (*search_p == '\n')
			return false;
		search_p++;
		if (search_p == script->end_p)
			return false;

	}

	if (*search_p == ';' || *search_p == '#' ||		 // semicolon and # is comment field
		(*search_p == '/' && *((search_p)+1) == '/')) // also make // a comment field
		return false;

	return true;
}

/*
==============
PeekToken

Non-destructive lookahead: copies the next real token (skipping whitespace and
//, #, ; and slash-star comments, crossing line boundaries) into buffer WITHOUT
advancing script_p and WITHOUT touching the tokenready/token globals. If a token
is already pending from UnGetToken it returns that. Returns true if a token was
found, false at end of script.

Word boundaries match GetToken (a token runs until whitespace or ';'), so e.g.
'{' on its own is returned as "{".
==============
*/
qboolean PeekToken( char *buffer, int bufferSize )
{
	if ( bufferSize > 0 )
		buffer[0] = '\0';

	if ( tokenready )                         // pending UnGetToken
	{
		Q_strncpy( buffer, token, bufferSize );
		return true;
	}

	if ( !script )
		return false;

	const char *p = script->script_p;
	while ( p < script->end_p )
	{
		if ( (unsigned char)*p <= ' ' ) { p++; continue; }
		if ( (*p == '/' && p+1 < script->end_p && *(p+1) == '/') || *p == '#' || *p == ';' )
		{
			while ( p < script->end_p && *p != '\n' ) p++;
			continue;
		}
		if ( *p == '/' && p+1 < script->end_p && *(p+1) == '*' )
		{
			p += 2;
			while ( p+1 < script->end_p && !(*p == '*' && *(p+1) == '/') ) p++;
			if ( p+1 < script->end_p ) p += 2;
			continue;
		}
		break;
	}

	if ( p >= script->end_p )
		return false;

	int len = 0;
	while ( p < script->end_p && (unsigned char)*p > ' ' && *p != ';' && len < bufferSize - 1 )
		buffer[len++] = *p++;
	buffer[len] = '\0';
	return len > 0;
}

// Non-consuming lookahead: if the next significant char is '{', scan the balanced block for
// a bare word == needle (case-insensitive), skipping quotes/comments. Walks a local cursor
// copy only, so the tokenizer state is untouched. Returns false if the next char isn't '{'.
qboolean PeekBlockContainsToken( const char *needle )
{
	if ( !script || tokenready || !needle || !needle[0] )
		return false;

	const char *p   = script->script_p;
	const char *end = script->end_p;

	// Skip whitespace / comments to the next significant character.
	while ( p < end )
	{
		if ( (unsigned char)*p <= ' ' ) { p++; continue; }
		if ( (*p == '/' && p+1 < end && *(p+1) == '/') || *p == '#' || *p == ';' )
		{ while ( p < end && *p != '\n' ) p++; continue; }
		if ( *p == '/' && p+1 < end && *(p+1) == '*' )
		{ p += 2; while ( p+1 < end && !(*p == '*' && *(p+1) == '/') ) p++; if ( p+1 < end ) p += 2; continue; }
		break;
	}

	if ( p >= end || *p != '{' )
		return false;
	p++; // enter the block

	const int needleLen = (int)strlen( needle );
	int depth = 1;
	while ( p < end && depth > 0 )
	{
		char c = *p;
		if ( c == '"' )
		{
			p++;
			while ( p < end && *p != '"' ) p++;
			if ( p < end ) p++;
			continue;
		}
		if ( (c == '/' && p+1 < end && *(p+1) == '/') || c == '#' || c == ';' )
		{ while ( p < end && *p != '\n' ) p++; continue; }
		if ( c == '/' && p+1 < end && *(p+1) == '*' )
		{ p += 2; while ( p+1 < end && !(*p == '*' && *(p+1) == '/') ) p++; if ( p+1 < end ) p += 2; continue; }
		if ( c == '{' ) { depth++; p++; continue; }
		if ( c == '}' ) { depth--; p++; continue; }
		if ( (unsigned char)c <= ' ' ) { p++; continue; }

		// Start of a bare word - measure it, then compare against the needle.
		const char *wstart = p;
		while ( p < end )
		{
			char wc = *p;
			if ( (unsigned char)wc <= ' ' || wc == '{' || wc == '}' || wc == '"' || wc == ';' )
				break;
			p++;
		}
		int wlen = (int)( p - wstart );
		if ( wlen == needleLen && Q_strnicmp( wstart, needle, needleLen ) == 0 )
			return true;
	}
	return false;
}

qboolean GetTokenizerStatus( char **pFilename, int *pLine )
{
	// is this the default state?
	if (!script)
		return false;

	if (script->script_p >= script->end_p)
		return false;

	if (pFilename)
	{
		*pFilename = script->filename;
	}
	if (pLine)
	{
		*pLine = script->line;
	}
	return true;
}


#include <stdio.h>
#include <stdlib.h>
#ifdef WIN32
#include <direct.h>
#include <io.h>
#include <sys/utime.h>
#endif
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "tier1/utlbuffer.h"

class CScriptLib : public IScriptLib
{
public:
	virtual bool ReadFileToBuffer( const char *pSourceName, CUtlBuffer &buffer, bool bText = false, bool bNoOpenFailureWarning = false );
	virtual bool WriteBufferToFile( const char *pTargetName, CUtlBuffer &buffer, DiskWriteMode_t writeMode );
	virtual int	FindFiles( char* pFileMask, bool bRecurse, CUtlVector<fileList_t> &fileList );
	virtual char *MakeTemporaryFilename( char const *pchModPath, char *pPath, int pathSize );
	virtual void DeleteTemporaryFiles( const char *pFileMask );
	virtual int CompareFileTime( const char *pFilenameA, const char *pFilenameB );
	virtual bool DoesFileExist( const char *pFilename );

private:

	int GetFileList( const char* pDirPath, const char* pPattern, CUtlVector< fileList_t > &fileList );
	void RecurseFileTree_r( const char* pDirPath, int depth, CUtlVector< CUtlString > &dirList );
};

static CScriptLib g_ScriptLib;
IScriptLib *scriptlib = &g_ScriptLib;
IScriptLib *g_pScriptLib = &g_ScriptLib;

//-----------------------------------------------------------------------------
// Existence check
//-----------------------------------------------------------------------------
bool CScriptLib::DoesFileExist( const char *pFilename )
{
	return g_pFullFileSystem->FileExists( pFilename );
}

//-----------------------------------------------------------------------------
// Purpose: Helper utility, read file into buffer
//-----------------------------------------------------------------------------
bool CScriptLib::ReadFileToBuffer( const char *pSourceName, CUtlBuffer &buffer, bool bText, bool bNoOpenFailureWarning )
{
	bool bSuccess = true;

	if ( !g_pFullFileSystem->ReadFile( pSourceName, NULL, buffer ) )
	{
		if ( !bNoOpenFailureWarning )
		{
			Msg( "ReadFileToBuffer(): Error opening %s: %s\n", pSourceName, strerror( errno ) );
		}
		return false;
	}

	if ( bText )
	{
		// force it into text mode
		buffer.SetBufferType( true, true );
	}
	else
	{
		buffer.SetBufferType( false, false );
	}

	return bSuccess;
}

//-----------------------------------------------------------------------------
// Purpose: Helper utility, Write buffer to file
//-----------------------------------------------------------------------------
bool CScriptLib::WriteBufferToFile( const char *pTargetName, CUtlBuffer &buffer, DiskWriteMode_t writeMode )
{
	char*	ptr;
	char	dirPath[MAX_PATH];

	bool bSuccess = true;

	// create path
	// prime and skip to first seperator
	strcpy( dirPath, pTargetName );
	ptr = strchr( dirPath, '\\' );
	while ( ptr )
	{		
		ptr = strchr( ptr+1, '\\' );
		if ( ptr )
		{
			*ptr = '\0';
			_mkdir( dirPath );
			*ptr = '\\';
		}
	}

	bool bDoWrite = false;
	if ( writeMode == WRITE_TO_DISK_ALWAYS )
	{
		bDoWrite = true;
	}
	else if ( writeMode == WRITE_TO_DISK_UPDATE )
	{
		if ( DoesFileExist( pTargetName ) )
		{
			bDoWrite = true;
		}
	}

	if ( bDoWrite )
	{
		bSuccess = g_pFullFileSystem->WriteFile( pTargetName, NULL, buffer );
	}

	return bSuccess;
}

//-----------------------------------------------------------------------------
// Returns -1, 0, or 1.
//-----------------------------------------------------------------------------
int CScriptLib::CompareFileTime( const char *pFilenameA, const char *pFilenameB )
{
	int timeA = g_pFullFileSystem->GetFileTime( (char *)pFilenameA );
	int timeB = g_pFullFileSystem->GetFileTime( (char *)pFilenameB );

	if ( timeA == -1)
	{
		// file a not exist
		timeA = 0;
	}
	if ( timeB == -1 )
	{
		// file b not exist
		timeB = 0;
	}

	if ( (unsigned int)timeA < (unsigned int)timeB )
	{
		return -1;
	}
	else if ( (unsigned int)timeA > (unsigned int)timeB )
	{
		return 1;
	}

	return 0;
}

//-----------------------------------------------------------------------------
// Make a temporary filename
//-----------------------------------------------------------------------------
char *CScriptLib::MakeTemporaryFilename( char const *pchModPath, char *pPath, int pathSize )
{
	char *pBuffer = _tempnam( pchModPath, "mgd_" );
	if ( pBuffer[0] == '\\' )
	{
		pBuffer++;
	}
	if ( pBuffer[strlen( pBuffer )-1] == '.' )
	{
		pBuffer[strlen( pBuffer )-1] = '\0';
	}
	V_snprintf( pPath, pathSize, "%s.tmp", pBuffer );

	free( pBuffer );

	return pPath;
}

//-----------------------------------------------------------------------------
// Delete temporary files
//-----------------------------------------------------------------------------
void CScriptLib::DeleteTemporaryFiles( const char *pFileMask )
{
#if !defined( _X360 )
	const char *pEnv = getenv( "temp" );
	if ( !pEnv )
	{
		pEnv = getenv( "tmp" );
	}

	if ( pEnv )
	{
		char tempPath[MAX_PATH];
		strcpy( tempPath, pEnv );
		V_AppendSlash( tempPath, sizeof( tempPath ) );
		strcat( tempPath, pFileMask );

		CUtlVector<fileList_t> fileList;
		FindFiles( tempPath, false, fileList );
		for ( int i=0; i<fileList.Count(); i++ )
		{
			_unlink( fileList[i].fileName.String() );
		}
	}
#else
	AssertOnce( !"CScriptLib::DeleteTemporaryFiles:  Not avail on 360\n" );
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Get list of files from current path that match pattern
//-----------------------------------------------------------------------------
int CScriptLib::GetFileList( const char* pDirPath, const char* pPattern, CUtlVector< fileList_t > &fileList )
{
	char	sourcePath[MAX_PATH];
	char	fullPath[MAX_PATH];
	bool	bFindDirs;

	fileList.Purge();

	strcpy( sourcePath, pDirPath );
	int len = (int)strlen( sourcePath );
	if ( !len )
	{
		strcpy( sourcePath, ".\\" );
	}
	else if ( sourcePath[len-1] != '\\' )
	{
		sourcePath[len]   = '\\';
		sourcePath[len+1] = '\0';
	}

	strcpy( fullPath, sourcePath );
	if ( pPattern[0] == '\\' && pPattern[1] == '\0' )
	{
		// find directories only
		bFindDirs = true;
		strcat( fullPath, "*" );
	}
	else
	{
		// find files, use provided pattern
		bFindDirs = false;
		strcat( fullPath, pPattern );
	}

#ifdef WIN32
	struct _finddata_t findData;
	intptr_t h = _findfirst( fullPath, &findData );
	if ( h == -1 )
	{
		return 0;
	}

	do
	{
		// dos attribute complexities i.e. _A_NORMAL is 0
		if ( bFindDirs )
		{
			// skip non dirs
			if ( !( findData.attrib & _A_SUBDIR ) )
				continue;
		}
		else
		{
			// skip dirs
			if ( findData.attrib & _A_SUBDIR )
				continue;
		}

		if ( !stricmp( findData.name, "." ) )
			continue;

		if ( !stricmp( findData.name, ".." ) )
			continue;

		char fileName[MAX_PATH];
		strcpy( fileName, sourcePath );
		strcat( fileName, findData.name );

		int j = fileList.AddToTail();
		fileList[j].fileName.Set( fileName );
		fileList[j].timeWrite = findData.time_write;
	}
	while ( !_findnext( h, &findData ) );

	_findclose( h );
#elif defined(POSIX)
	FIND_DATA findData;
	Q_FixSlashes( fullPath );
	void *h = FindFirstFile( fullPath, &findData );
	if ( (intp)h == -1 )
	{
		return 0;
	}

	do
	{
		// dos attribute complexities i.e. _A_NORMAL is 0
		if ( bFindDirs )
		{
			// skip non dirs
			if ( !( findData.dwFileAttributes & S_IFDIR ) )
				continue;
		}
		else
		{
			// skip dirs
			if ( findData.dwFileAttributes & S_IFDIR )
				continue;
		}

		if ( !stricmp( findData.cFileName, "." ) )
			continue;

		if ( !stricmp( findData.cFileName, ".." ) )
			continue;

		char fileName[MAX_PATH];
		strcpy( fileName, sourcePath );
		strcat( fileName, findData.cFileName );

		int j = fileList.AddToTail();
		fileList[j].fileName.Set( fileName );
		struct stat statbuf;
		if ( stat( fileName, &statbuf ) )
#ifdef LINUX
			fileList[j].timeWrite = statbuf.st_mtime;
#else
			fileList[j].timeWrite = statbuf.st_mtimespec.tv_sec;
#endif
		else
			fileList[j].timeWrite = 0;
	}
	while ( !FindNextFile( h, &findData ) );

	FindClose( h );

#else
#error
#endif
	

	return fileList.Count();
}

//-----------------------------------------------------------------------------
// Purpose: Recursively determine directory tree
//-----------------------------------------------------------------------------
void CScriptLib::RecurseFileTree_r( const char* pDirPath, int depth, CUtlVector< CUtlString > &dirList )
{
	// recurse from source directory, get directories only
	CUtlVector< fileList_t > fileList;
	int dirCount = GetFileList( pDirPath, "\\", fileList );
	if ( !dirCount )
	{
		// add directory name to search tree
		int j = dirList.AddToTail();
		dirList[j].Set( pDirPath );
		return;
	}

	for ( int i=0; i<dirCount; i++ )
	{
		// form new path name, recurse into
		RecurseFileTree_r( fileList[i].fileName.String(), depth+1, dirList );
	}

	int j = dirList.AddToTail();
	dirList[j].Set( pDirPath );
}

//-----------------------------------------------------------------------------
// Purpose: Generate a list of file matching mask
//-----------------------------------------------------------------------------
int CScriptLib::FindFiles( char* pFileMask, bool bRecurse, CUtlVector<fileList_t> &fileList )
{
	char	dirPath[MAX_PATH];
	char	pattern[MAX_PATH];
	char	extension[MAX_PATH];

	// get path only
	strcpy( dirPath, pFileMask );
	V_StripFilename( dirPath );

	// get pattern only
	V_FileBase( pFileMask, pattern, sizeof( pattern ) );
	V_ExtractFileExtension( pFileMask, extension, sizeof( extension ) );
	if ( extension[0] )
	{
		strcat( pattern, "." );
		strcat( pattern, extension );
	}

	if ( !bRecurse )
	{
		GetFileList( dirPath, pattern, fileList );
	}
	else
	{
		// recurse and get the tree
		CUtlVector< fileList_t > tempList;
		CUtlVector< CUtlString > dirList;
		RecurseFileTree_r( dirPath, 0, dirList );
		for ( int i=0; i<dirList.Count(); i++ )
		{
			// iterate each directory found
			tempList.Purge();
			tempList.EnsureCapacity( dirList.Count() );

			GetFileList( dirList[i].String(), pattern, tempList );

			int start = fileList.AddMultipleToTail( tempList.Count() );
			for ( int j=0; j<tempList.Count(); j++ )
			{
				fileList[start+j] = tempList[j];
			}
		}	
	}

	return fileList.Count();
}
