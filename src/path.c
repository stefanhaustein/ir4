/* path.c - Path helpers and executable directory lookup */
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include "ir4.h"
#include "path.h"

static StrBuf path_scratch;
static const char *exe_dir = NULL;

const char *path_join(const char *a, const char *b) {
    sb_reset(&path_scratch);
    sb_append(&path_scratch, a);
    sb_putc(&path_scratch, '/');
    sb_append(&path_scratch, b);
    return sb_intern(&path_scratch);
}

const char *path_resolve(const char *base_path, const char *rel) {
    const char *slash;
    size_t dir_len = 0;

    while (rel[0] == '.' && rel[1] == '/') {
        rel += 2;
    }

    if (rel[0] != '/' && base_path != NULL) {
        slash = strrchr(base_path, '/');
        if (slash != NULL) {
            dir_len = (size_t)(slash - base_path + 1);
        }
    }

    if (dir_len == 0)
        return intern_str(rel);
    sb_reset(&path_scratch);
    sb_append_n(&path_scratch, base_path, dir_len);
    sb_append(&path_scratch, rel);
    return sb_intern(&path_scratch);
}

static void set_exe_dir_from_path(const char *exe_path) {
    const char *sep = NULL;
    const char *p;
    size_t len;
    exe_dir = NULL;
    if (!exe_path)
        return;
    for (p = exe_path; *p; p++) {
        if (*p == '/' || *p == '\\')
            sep = p;
    }
    if (!sep)
        return;
    len = (size_t)(sep - exe_path);
    exe_dir = (len == 0) ? intern_str("/") : intern_str_n(exe_path, len);
}

void path_set_exe(const char *argv0) {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    if (size > 0) {
        VEC_ENSURE(path_scratch, (size_t)size);
        if (_NSGetExecutablePath(path_scratch.data, &size) == 0) {
            char *resolved = realpath(path_scratch.data, NULL);
            set_exe_dir_from_path(resolved ? resolved : path_scratch.data);
            free(resolved);
            return;
        }
    }
#elif defined(__linux__)
    size_t cap = 256;
    for (;;) {
        ssize_t n;
        VEC_ENSURE(path_scratch, cap);
        n = readlink("/proc/self/exe", path_scratch.data, path_scratch.cap);
        if (n < 0)
            break;
        if ((size_t)n < path_scratch.cap) {
            path_scratch.data[n] = '\0';
            set_exe_dir_from_path(path_scratch.data);
            return;
        }
        cap *= 2;
    }
#elif defined(_WIN32)
    DWORD cap = 256;
    for (;;) {
        DWORD n;
        VEC_ENSURE(path_scratch, (size_t)cap);
        n = GetModuleFileNameA(NULL, path_scratch.data, (DWORD)path_scratch.cap);
        if (n == 0)
            break;
        if (n < (DWORD)path_scratch.cap) {
            set_exe_dir_from_path(path_scratch.data);
            return;
        }
        cap *= 2;
    }
#endif
    set_exe_dir_from_path(argv0);
}

const char *path_exe_dir(void) { return exe_dir; }
