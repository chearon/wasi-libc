///! Support for "preopens", file descriptors passed into the program from the
///! environment, with associated path prefixes, which can be used to map
///! absolute paths to capabilities with relative paths.

#ifdef _REENTRANT
#error "__wasilibc_register_preopened_fd doesn't yet support multiple threads"
#endif

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <wasi/api.h>
#include <wasi/libc.h>
#include <wasi/libc-find-relpath.h>

/// A name and file descriptor pair.
typedef struct preopen {
    /// The path prefix associated with the file descriptor.
    const char *prefix;

    /// The file descriptor.
    __wasi_fd_t fd;
} preopen;

/// A simple growable array of `preopen`.
static preopen *preopens;
static size_t num_preopens;
static size_t preopen_capacity;

#ifdef NDEBUG
#define assert_invariants() // assertions disabled
#else
static void assert_invariants(void) {
    assert(num_preopens <= preopen_capacity);
    assert(preopen_capacity == 0 || preopens != NULL);
    assert(preopen_capacity == 0 ||
           preopen_capacity * sizeof(preopen) > preopen_capacity);

    for (size_t i = 0; i < num_preopens; ++i) {
        const preopen *pre = &preopens[i];
        assert(pre->prefix != NULL);
        assert(pre->fd != (__wasi_fd_t)-1);
#ifdef __wasm__
        assert((uintptr_t)pre->prefix <
               (__uint128_t)__builtin_wasm_memory_size(0) * PAGESIZE);
#endif
    }
}
#endif

/// Allocate space for more preopens. Returns 0 on success and -1 on failure.
static int resize(void) {
    size_t start_capacity = 4;
    size_t old_capacity = preopen_capacity;
    size_t new_capacity = old_capacity == 0 ? start_capacity : old_capacity * 2;

    preopen *old_preopens = preopens;
    preopen *new_preopens = calloc(sizeof(preopen), new_capacity);
    if (new_preopens == NULL)
        return -1;

    memcpy(new_preopens, old_preopens, num_preopens * sizeof(preopen));
    preopens = new_preopens;
    preopen_capacity = new_capacity;
    free(old_preopens);

    assert_invariants();
    return 0;
}

/// Register the given preopened file descriptor under the given path.
///
/// This function takes ownership of `prefix`.
static int internal_register_preopened_fd(__wasi_fd_t fd, const char *prefix) {
    assert_invariants();

    if (num_preopens == preopen_capacity && resize() != 0)
        return -1;

    preopens[num_preopens++] = (preopen) { prefix, fd, };

    assert_invariants();
    return 0;
}

/// Are the `prefix_len` bytes pointed to by `prefix` a prefix of `path`?
static bool prefix_matches(const char *prefix, size_t prefix_len, const char *path) {
    // Allow an empty string as a prefix of any relative path.
    if (path[0] != '/' && prefix_len == 0)
        return true;

    // Check whether any bytes of the prefix differ.
    if (memcmp(path, prefix, prefix_len) != 0)
        return false;

    // Ignore trailing slashes in directory names.
    size_t i = prefix_len;
    while (i > 0 && prefix[i - 1] == '/') {
        --i;
    }

    // Match only complete path components.
    char last = path[i];
    return last == '/' || last == '\0';
}

// See the documentation in libc.h
int __wasilibc_register_preopened_fd(int fd, const char *prefix) {
    prefix = strdup(prefix);
    return prefix == NULL ? -1 :
           internal_register_preopened_fd((__wasi_fd_t)fd, prefix);
}

// See the documentation in libc-find-relpath.h.
int __wasilibc_find_relpath(const char *path,
                            const char **restrict relative_path) {
    assert_invariants();

    // Search through the preopens table. Iterate in reverse so that more
    // recently added preopens take precedence over less recently addded ones.
    size_t match_len = 0;
    int fd = -1;
    for (size_t i = num_preopens; i > 0; --i) {
        const preopen *pre = &preopens[i - 1];
        const char *prefix = pre->prefix;
        size_t len = strlen(prefix);

        if (path[0] != '/' &&
            (path[0] != '.' || (path[1] != '/' && path[1] != '\0')))
        {
            // We're matching a relative path that doesn't start with "./" and
            // isn't ".".
            if (len >= 2 && prefix[0] == '.' && prefix[1] == '/') {
                // The preopen starts with "./", so skip that prefix.
                prefix += 2;
                len -= 2;
            } else if (len == 1 && prefix[0] == '.') {
                // The preopen is ".", so match it as an empty string.
                prefix += 1;
                len -= 1;
            }
        }

        // If we haven't had a match yet, or the candidate path is longer than
        // our current best match's path, and the candidate path is a prefix of
        // the requested path, take that as the new best path.
        if ((fd == -1 || len > match_len) &&
            prefix_matches(prefix, len, path))
        {
            fd = pre->fd;
            match_len = len;
        }
    }

    // The relative path is the substring after the portion that was matched.
    const char *computed = path + match_len;

    // Omit leading slashes in the relative path.
    while (*computed == '/')
        ++computed;

    // *at syscalls don't accept empty relative paths, so use "." instead.
    if (*computed == '\0')
        computed = ".";

    *relative_path = computed;
    return fd;
}

/// This is referenced by weak reference from crt1.c and lives in the same
/// source file as `__wasilibc_find_relpath` so that it's linked in when it's
/// needed.
// Concerning the 51 -- see the comment by the constructor priority in
// libc-bottom-half/sources/__environ.c.
__attribute__((constructor(51)))
static void __wasilibc_populate_libpreopen(void) {
    // Skip stdin, stdout, and stderr, and count up until we reach an invalid
    // file descriptor.
    for (__wasi_fd_t fd = 3; fd != 0; ++fd) {
        __wasi_prestat_t prestat;
        __wasi_errno_t ret = __wasi_fd_prestat_get(fd, &prestat);
        if (ret == __WASI_ERRNO_BADF)
            break;
        if (ret != __WASI_ERRNO_SUCCESS)
            goto oserr;
        switch (prestat.tag) {
        case __WASI_PREOPENTYPE_DIR: {
            char *prefix = malloc(prestat.u.dir.pr_name_len + 1);
            if (prefix == NULL)
                goto software;

            // TODO: Remove the cast on `path` once the witx is updated with
            // char8 support.
            ret = __wasi_fd_prestat_dir_name(fd, (uint8_t *)prefix,
                                             prestat.u.dir.pr_name_len);
            if (ret != __WASI_ERRNO_SUCCESS)
                goto oserr;
            prefix[prestat.u.dir.pr_name_len] = '\0';

            if (internal_register_preopened_fd(fd, prefix) != 0)
                goto software;

            break;
        }
        default:
            break;
        }
    }

    return;
oserr:
    _Exit(EX_OSERR);
software:
    _Exit(EX_SOFTWARE);
}
