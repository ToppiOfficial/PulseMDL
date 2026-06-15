//
// Created by RED on 15.05.2024.
//

#include <processthreadsapi.h>
#include <errhandlingapi.h>
#include <libloaderapi.h>
#include "studiomdl_errors.h"
#include "common/scriplib.h"
#include "studiomdl/studiomdl.h"
#include "datamodel/idatamodel.h"

extern StudioMdlContext g_StudioMdlContext;
static bool g_bFirstWarning = true;

void TokenError(const char *fmt, ...) {
    static char output[1024];
    va_list args;

    char *pFilename;
    int iLineNumber;

    if (GetTokenizerStatus(&pFilename, &iLineNumber)) {
                va_start(args, fmt);
        vsprintf(output, fmt, args);

        MdlError("%s(%d): - %s", pFilename, iLineNumber, output);
    } else {
                va_start(args, fmt);
        vsprintf(output, fmt, args);
        MdlError("%s", output);
    }
}

void MdlError(const char *fmt, ...) {
    static char output[1024];
    static char *knownExtensions[] = {".mdl", ".ani", ".phy", ".sw.vtx", ".dx80.vtx", ".dx90.vtx", ".vvd"};
    char fileName[MAX_PATH];
    char baseName[MAX_PATH];
    va_list args;

//	Assert( 0 );
    if (g_StudioMdlContext.quiet) {
        if (g_bFirstWarning) {
            printf("%s :\n", g_fullpath);
            g_bFirstWarning = false;
        }
        printf("\t");
    }

    printf("ERROR: ");
            va_start(args, fmt);
    vprintf(fmt, args);

    // delete premature files
    // unforunately, content is built without verification
    // ensuring that targets are not available, prevents check-in
    if (g_StudioMdlContext.bHasModelName) {
        if (g_StudioMdlContext.quiet) {
            printf("\t");
        }

        // undescriptive errors in batch processes could be anonymous
        printf("ERROR: Aborted Processing on '%s'\n", g_outname);

        strcpy(fileName, gamedir);
        strcat(fileName, "models/");
        strcat(fileName, g_outname);
        Q_FixSlashes(fileName);
        Q_StripExtension(fileName, baseName, sizeof(baseName));

        for (int i = 0; i < ARRAYSIZE(knownExtensions); i++) {
            strcpy(fileName, baseName);
            strcat(fileName, knownExtensions[i]);

            // really need filesystem concept here
//			g_pFileSystem->RemoveFile( fileName );
            unlink(fileName);
        }
    }

    for (int i = 0; i < g_pDataModel->NumFileIds(); ++i) {
        g_pDataModel->UnloadFile(g_pDataModel->GetFileId(i));
    }

    if (g_StudioMdlContext.parseable_completion_output) {
        printf("\nRESULT: ERROR\n");
    }

    exit(-1);
}

void MdlWarning(const char *fmt, ...) {
    va_list args;
    static char output[1024];

    if (g_StudioMdlContext.bNoWarnings || g_StudioMdlContext.g_maxWarnings == 0)
        return;

    ushort old = SetConsoleTextColor(1, 1, 0, 1);

    if (g_StudioMdlContext.quiet) {
        if (g_bFirstWarning) {
            printf("%s :\n", g_fullpath);
            g_bFirstWarning = false;
        }
        printf("\t");
    }

    //Assert( 0 );

    printf("WARNING: ");
            va_start(args, fmt);
    vprintf(fmt, args);

    if (g_StudioMdlContext.g_maxWarnings > 0)
        g_StudioMdlContext.g_maxWarnings--;

    if (g_StudioMdlContext.g_maxWarnings == 0) {
        if (g_StudioMdlContext.quiet) {
            printf("\t");
        }
        printf("suppressing further warnings...\n");
    }

    RestoreConsoleTextColor(old);
}

void CMdlLoggingListener::Log(const LoggingContext_t *pContext, const tchar *pMessage) {
    if (pContext->m_Severity == LS_MESSAGE && g_StudioMdlContext.quiet) {
        // suppress
    } else if (pContext->m_Severity == LS_WARNING) {
        MdlWarning("%s", pMessage);
    } else {
        CCmdLibStandardLoggingListener::Log(pContext, pMessage);
    }
}

//#ifndef _DEBUG

// Resolve a code address to "module.dll+0xOFFSET". Returns false if it can't.
static bool MdlResolveModule(void *pAddr, char *pOut, size_t outLen) {
    HMODULE hMod = NULL;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR) pAddr, &hMod) || !hMod)
        return false;

    char path[MAX_PATH];
    if (!GetModuleFileNameA(hMod, path, sizeof(path)))
        return false;

    // keep just the file name, not the full path
    const char *pName = path;
    for (const char *p = path; *p; ++p)
        if (*p == '\\' || *p == '/')
            pName = p + 1;

    uintptr_t offset = (uintptr_t) pAddr - (uintptr_t) hMod;
    Q_snprintf(pOut, outLen, "%s+0x%llX", pName, (unsigned long long) offset);
    return true;
}

// pAddr   - faulting instruction address (ExceptionAddress)
// pDetail - optional extra clause (e.g. "reading address 0x...") or NULL
void MdlHandleCrash(const char *pMessage, void *pAddr, const char *pDetail, bool bAssert) {
    char module[MAX_PATH + 32];
    if (pAddr && MdlResolveModule(pAddr, module, sizeof(module))) {
        MdlError("'%s' at 0x%p (%s)%s%s (assert: %d)\n",
                 pMessage, pAddr, module,
                 pDetail ? " " : "", pDetail ? pDetail : "",
                 bAssert);
    } else if (pAddr) {
        MdlError("'%s' at 0x%p%s%s (assert: %d)\n",
                 pMessage, pAddr,
                 pDetail ? " " : "", pDetail ? pDetail : "",
                 bAssert);
    } else {
        MdlError("'%s' (assert: %d)\n", pMessage, bAssert);
    }
}

// This is called if we crash inside our crash handler. It just terminates the process immediately.
LONG __stdcall MdlSecondExceptionFilter(struct _EXCEPTION_POINTERS *ExceptionInfo) {
    TerminateProcess(GetCurrentProcess(), 2);
    return EXCEPTION_EXECUTE_HANDLER; // (never gets here anyway)
}

void MdlExceptionFilter(struct _EXCEPTION_POINTERS *ExceptionInfo) {
    // This is called if we crash inside our crash handler. It just terminates the process immediately.
    SetUnhandledExceptionFilter(MdlSecondExceptionFilter);

    unsigned long code = ExceptionInfo->ExceptionRecord->ExceptionCode;
    void *pAddr = ExceptionInfo->ExceptionRecord->ExceptionAddress;

    // For an access violation, ExceptionInformation[0] is the operation
    // (0 read, 1 write, 8 DEP) and [1] is the address that was accessed.
    char detail[64];
    const char *pDetail = NULL;
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ExceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR op = ExceptionInfo->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR bad = ExceptionInfo->ExceptionRecord->ExceptionInformation[1];
        const char *pVerb = (op == 1) ? "writing" : (op == 8) ? "executing" : "reading";
        Q_snprintf(detail, sizeof(detail), "%s address 0x%p", pVerb, (void *) bad);
        pDetail = detail;
    }

#define ERR_RECORD(name) { name, #name }
    struct {
        unsigned long code;
        const char *pReason;
    } errors[] =
            {
                    ERR_RECORD(EXCEPTION_ACCESS_VIOLATION),
                    ERR_RECORD(EXCEPTION_ARRAY_BOUNDS_EXCEEDED),
                    ERR_RECORD(EXCEPTION_BREAKPOINT),
                    ERR_RECORD(EXCEPTION_DATATYPE_MISALIGNMENT),
                    ERR_RECORD(EXCEPTION_FLT_DENORMAL_OPERAND),
                    ERR_RECORD(EXCEPTION_FLT_DIVIDE_BY_ZERO),
                    ERR_RECORD(EXCEPTION_FLT_INEXACT_RESULT),
                    ERR_RECORD(EXCEPTION_FLT_INVALID_OPERATION),
                    ERR_RECORD(EXCEPTION_FLT_OVERFLOW),
                    ERR_RECORD(EXCEPTION_FLT_STACK_CHECK),
                    ERR_RECORD(EXCEPTION_FLT_UNDERFLOW),
                    ERR_RECORD(EXCEPTION_ILLEGAL_INSTRUCTION),
                    ERR_RECORD(EXCEPTION_IN_PAGE_ERROR),
                    ERR_RECORD(EXCEPTION_INT_DIVIDE_BY_ZERO),
                    ERR_RECORD(EXCEPTION_INT_OVERFLOW),
                    ERR_RECORD(EXCEPTION_INVALID_DISPOSITION),
                    ERR_RECORD(EXCEPTION_NONCONTINUABLE_EXCEPTION),
                    ERR_RECORD(EXCEPTION_PRIV_INSTRUCTION),
                    ERR_RECORD(EXCEPTION_SINGLE_STEP),
                    ERR_RECORD(EXCEPTION_STACK_OVERFLOW),
                    ERR_RECORD(EXCEPTION_ACCESS_VIOLATION),
            };

    int nErrors = sizeof(errors) / sizeof(errors[0]);
    {
        int i;
        for (i = 0; i < nErrors; i++) {
            if (errors[i].code == code)
                MdlHandleCrash(errors[i].pReason, pAddr, pDetail, true);
        }

        if (i == nErrors) {
            MdlHandleCrash("Unknown reason", pAddr, pDetail, true);
        }
    }

    TerminateProcess(GetCurrentProcess(), 1);
}

LONG __stdcall VExceptionFilter(struct _EXCEPTION_POINTERS *ExceptionInfo) {
    MdlExceptionFilter(ExceptionInfo);
    return EXCEPTION_EXECUTE_HANDLER; // (never gets here anyway)
}