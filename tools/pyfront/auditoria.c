#include <Python.h>

/* La API pública no permite consultar la presencia de hooks. Con versiones o
   encabezados desconocidos se conserva la apertura de CPython. Las cabeceras
   internas se aíslan aquí porque en Windows incluyen windows.h. */
#if PY_VERSION_HEX >= 0x030b0000 && PY_VERSION_HEX < 0x030f0000 && \
    !defined(Py_GIL_DISABLED) && !defined(WITH_DTRACE) && \
    __has_include("internal/pycore_interp.h") && __has_include("internal/pycore_runtime.h")
#define Py_BUILD_CORE
#include "internal/pycore_interp.h"
#include "internal/pycore_runtime.h"
#undef Py_BUILD_CORE
#define TZ_FILE_CHECK_AUDIT 1
#endif

int tz_file_without_audit(void) {
#ifdef TZ_FILE_CHECK_AUDIT
    PyInterpreterState* interp = PyInterpreterState_Get();
#if PY_VERSION_HEX < 0x030c0000
    return !interp->audit_hooks && !interp->runtime->audit_hook_head;
#else
    return !interp->audit_hooks && !interp->runtime->audit_hooks.head;
#endif
#else
    return 0;
#endif
}
