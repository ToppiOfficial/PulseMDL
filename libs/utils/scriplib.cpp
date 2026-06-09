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

	// find end of macro def
	while (*cp && *cp != '\n')
	{
		//Msg("%d ", *cp );
		if (*cp == '\\' && *(cp+1) == '\\')
		{
			// skip till end of line
			while (*cp && *cp != '\n')
			{
				*cp = ' '; // replace with spaces
				cp++;
			}

			if (*cp)
			{
				cp++;
			}
		}
		else
		{
			cp++;
		}
	}

	int size = (cp - script->script_p);

	pmacro->buffer = (char *)malloc( size + 1);
	memcpy( pmacro->buffer, script->script_p, size );
	pmacro->buffer[size] = '\0';
	pmacro->end_p = &pmacro->buffer[size]; 

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
		if ( !V_strcmp( g_definevariable[i].value, v.value ) )
		{
			Warning( "\"$definevariable %s %s\" already exists as \"%s\".", v.param_lcase, v.value, g_definevariable[i].param_lcase );
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
static const int k_MaxConditionalNesting = 3;
static const int k_MaxElifCount = 8;
static const int k_MaxCaseCount = 24;

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

#define SCRIPLIB_MAX_COND_TOKENS 64
typedef struct {
    char toks[SCRIPLIB_MAX_COND_TOKENS][MAXTOKEN];
    int  count;
} CondToks;

static void CondToks_Init(CondToks* c) { c->count = 0; }

static void CondToks_Push(CondToks* c, const char* tok) {
    if (c->count >= SCRIPLIB_MAX_COND_TOKENS)
        Error("$if: too many tokens in condition (max %d)\n", SCRIPLIB_MAX_COND_TOKENS);
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
    script++;
    if (script == &scriptstack[MAX_INCLUDES])
        Error("script file exceeded MAX_INCLUDES");
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
        V_strncpy(subct.toks[0], inner, MAXTOKEN); subct.count = 1;
        CondEval sub; sub.ct = &subct; sub.pos = 0;
        char tmp[MAXTOKEN]; tmp[0] = '\0';
        bool r = ce_evalAtom(&sub, tmp, sizeof(tmp));
        bool neg = !r;
        V_strncpy(outBuf, neg ? "1" : "0", outBufSize);
        return neg;
    }

    e->pos++;
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

// Read condition tokens via GetToken until '{' (which is consumed).
static bool s_readConditionTokens(CondToks* toks) {
    while (GetToken(true)) {
        if (!stricmp(token, "{")) return true;
        if (endofscript) { Error("$if: missing '{'\n"); return false; }
        CondToks_Push(toks, token);
    }
    Error("$if: missing '{'\n");
    return false;
}

static bool s_evalConditionTokens(CondToks* toks) {
    if (toks->count == 0) { Error("$if: empty condition\n"); return false; }
    CondEval ev; ev.ct = toks; ev.pos = 0;
    bool result = ce_evalOr(&ev);
    if (ev.pos < ev.ct->count)
        Error("$if: unexpected token '%s' in condition\n", ev.ct->toks[ev.pos]);
    return result;
}

static void ProcessIfDirective() {
    if (++s_conditionalDepth > k_MaxConditionalNesting) {
        Error("$if: maximum nesting depth (%d) exceeded\n", k_MaxConditionalNesting);
        --s_conditionalDepth; return;
    }

    bool taken = false;
    ScripBuf chosen; ScripBuf_Init(&chosen);
    int chosenLine = scriptline;
    int elifCount = 0;

    {
        CondToks toks; CondToks_Init(&toks);
        if (!s_readConditionTokens(&toks)) { --s_conditionalDepth; return; }
        bool cond = s_evalConditionTokens(&toks);
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
            if (++elifCount > k_MaxElifCount)
                Error("$if: maximum $elif count (%d) exceeded\n", k_MaxElifCount);
            CondToks toks; CondToks_Init(&toks);
            if (!s_readConditionTokens(&toks)) break;
            bool cond = s_evalConditionTokens(&toks);
            int bline = scriptline;
            ScripBuf block; ScripBuf_Init(&block);
            CollectBlockContentRaw(&block);
            if (cond && !taken) { taken = true; chosenLine = bline; ScripBuf_Steal(&chosen, &block); }
            ScripBuf_Free(&block);
        } else if (!Q_stricmp(peeked, "$else")) {
            GetToken(true); // consume the $else token
            GetToken(true); // consume '{'
            if (stricmp("{", token) != 0) { Error("$else: expected '{'\n"); break; }
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

static void ProcessSwitchDirective() {
    if (++s_conditionalDepth > k_MaxConditionalNesting) {
        Error("$switch: maximum nesting depth (%d) exceeded\n", k_MaxConditionalNesting);
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

    if (!GetToken(true) || stricmp("{", token) != 0) {
        Error("$switch: expected '{'\n"); --s_conditionalDepth; return;
    }

    bool caseTaken = false; bool hasDefault = false;
    int caseCount = 0;
    ScripBuf chosen; ScripBuf_Init(&chosen);
    int chosenLine = scriptline;

    while (true) {
        if (!GetToken(true) || endofscript) { Error("$switch: unexpected end of file\n"); break; }
        if (!stricmp("}", token)) break;

        if (!Q_stricmp("$case", token)) {
            if (++caseCount > k_MaxCaseCount)
                Error("$switch: maximum $case count (%d) exceeded\n", k_MaxCaseCount);
            if (!GetToken(false)) { Error("$case: expected value\n"); break; }
            char caseVal[MAXTOKEN]; V_strncpy(caseVal, token, sizeof(caseVal));
            if (!GetToken(true) || stricmp("{", token) != 0) { Error("$case: expected '{'\n"); break; }
            int bline = scriptline;
            ScripBuf block; ScripBuf_Init(&block);
            CollectBlockContentRaw(&block);
            if (!caseTaken && !strcmp(switchVal, caseVal)) {
                caseTaken = true; chosenLine = bline; ScripBuf_Steal(&chosen, &block);
            }
            ScripBuf_Free(&block);
        } else if (!Q_stricmp("$default", token)) {
            hasDefault = true;
            if (!GetToken(true) || stricmp("{", token) != 0) { Error("$default: expected '{'\n"); break; }
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
