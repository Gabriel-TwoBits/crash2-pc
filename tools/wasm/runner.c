/* Spike: runs wasm2c's crash2 module natively, with the part of WASI preview 1 the program uses: its arguments and
 * environment, files under one preopened root, the clocks, exit. The rest answers ENOSYS */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "crash2.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"

#define WASI_ESUCCESS 0
#define WASI_EBADF 8
#define WASI_EINVAL 28
#define WASI_EIO 29
#define WASI_ENOENT 44
#define WASI_ENOSYS 52
#define WASI_EEXIST 20
#define WASI_EISDIR 31
#define WASI_ENOTDIR 54
#define WASI_EACCES 2

#define FILETYPE_DIRECTORY 3
#define FILETYPE_REGULAR 4
#define FILETYPE_CHARACTER 2
#define OFLAG_CREAT 1
#define OFLAG_DIRECTORY 2
#define OFLAG_EXCL 4
#define OFLAG_TRUNC 8
#define FDFLAG_APPEND 1
#define RIGHT_FD_READ (1ull << 1)
#define RIGHT_FD_WRITE (1ull << 6)
#define ALL_RIGHTS 0x1FFFFFFFull
#define PREOPEN_FD 3
#define FDS_MAX 64
#define PATH_BYTES 4096
#define NS_PER_SECOND 1000000000ull

/* A path's bytes once the leading '/' and the terminating NUL are in PATH_BYTES */
#define PATH_NAME_BYTES (PATH_BYTES - 2)
#define WASI_WHENCE_SET 0
#define WASI_WHENCE_CUR 1
#define WASI_CLOCK_REALTIME 0
#define CLOCK_RESOLUTION_NS 1
/* A module's pointer, as args_get and environ_get list them */
#define WASM_POINTER_BYTES 4
/* prestat: the tag (a directory), then the length of its name, "/" */
#define WASI_PREOPENTYPE_DIR 0
#define PRESTAT_NAME_LENGTH_OFFSET 4
#define PREOPEN_NAME_LENGTH 1
/* iovec and ciovec: the buffer's pointer, then its length */
#define IOVEC_BYTES 8
#define IOVEC_LENGTH_OFFSET 4
/* filestat: dev, ino, filetype, nlink, size, then the times */
#define FILESTAT_BYTES 64
#define FILESTAT_INO_OFFSET 8
#define FILESTAT_FILETYPE_OFFSET 16
#define FILESTAT_NLINK_OFFSET 24
#define FILESTAT_SIZE_OFFSET 32
/* fdstat: filetype, flags, the base rights, the inheriting rights */
#define FDSTAT_BYTES 24
#define FDSTAT_RIGHTS_BASE_OFFSET 8
#define FDSTAT_RIGHTS_INHERITING_OFFSET 16
#define CREATED_FILE_MODE 0644
#define CREATED_DIRECTORY_MODE 0755

struct w2c_wasi__snapshot__preview1 {
    w2c_crash2 *module;
    int argc;
    char **argv;
    int host_fd[FDS_MAX];
    int is_directory[FDS_MAX];
};

extern char **environ;

static uint8_t *mem(struct w2c_wasi__snapshot__preview1 *w) {
    return w2c_crash2_memory(w->module)->data;
}

static void put32(struct w2c_wasi__snapshot__preview1 *w, u32 at, u32 value) { memcpy(mem(w) + at, &value, sizeof value); }
static void put64(struct w2c_wasi__snapshot__preview1 *w, u32 at, u64 value) { memcpy(mem(w) + at, &value, sizeof value); }

static u32 get32(struct w2c_wasi__snapshot__preview1 *w, u32 at) {
    u32 v;

    memcpy(&v, mem(w) + at, sizeof v);
    return v;
}

static u32 errno_to_wasi(int e) {
    switch (e) {
    case ENOENT: return WASI_ENOENT;
    case EEXIST: return WASI_EEXIST;
    case EISDIR: return WASI_EISDIR;
    case ENOTDIR: return WASI_ENOTDIR;
    case EACCES: return WASI_EACCES;
    case EBADF: return WASI_EBADF;
    case EINVAL: return WASI_EINVAL;
    default: return WASI_EIO;
    }
}

static int seek_whence(u32 whence) {
    switch (whence) {
    case WASI_WHENCE_SET: return SEEK_SET;
    case WASI_WHENCE_CUR: return SEEK_CUR;
    default: return SEEK_END;
    }
}

static uint8_t filetype(mode_t mode) {
    if (S_ISDIR(mode)) {
        return FILETYPE_DIRECTORY;
    } else if (S_ISREG(mode)) {
        return FILETYPE_REGULAR;
    } else {
        return FILETYPE_CHARACTER;
    }
}

static int host(struct w2c_wasi__snapshot__preview1 *w, u32 fd) {
    if (fd >= FDS_MAX) {
        return -1;
    } else {
        return w->host_fd[fd];
    }
}

/* The preopened root is the host's root: a module's path "x/y" is "/x/y" */
static void host_path(struct w2c_wasi__snapshot__preview1 *w, u32 path, u32 length, char *out) {
    u32 copied = length < PATH_NAME_BYTES ? length : PATH_NAME_BYTES;

    out[0] = '/';
    memcpy(out + 1, mem(w) + path, copied);
    out[1 + copied] = '\0';
}

u32 w2c_wasi__snapshot__preview1_args_sizes_get(struct w2c_wasi__snapshot__preview1 *w, u32 count, u32 bytes) {
    u32 total = 0;

    for (int i = 0; i < w->argc; i++) {
        total += strlen(w->argv[i]) + 1;
    }
    put32(w, count, w->argc);
    put32(w, bytes, total);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_args_get(struct w2c_wasi__snapshot__preview1 *w, u32 argv, u32 buffer) {
    for (int i = 0; i < w->argc; i++) {
        size_t n = strlen(w->argv[i]) + 1;

        memcpy(mem(w) + buffer, w->argv[i], n);
        put32(w, argv + WASM_POINTER_BYTES * i, buffer);
        buffer += n;
    }
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1 *w, u32 count, u32 bytes) {
    u32 n = 0;
    u32 total = 0;

    for (char **e = environ; *e != NULL; e++) {
        n++;
        total += strlen(*e) + 1;
    }
    put32(w, count, n);
    put32(w, bytes, total);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1 *w, u32 list, u32 buffer) {
    u32 i = 0;

    for (char **e = environ; *e != NULL; e++, i++) {
        size_t n = strlen(*e) + 1;

        memcpy(mem(w) + buffer, *e, n);
        put32(w, list + WASM_POINTER_BYTES * i, buffer);
        buffer += n;
    }
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_clock_res_get(struct w2c_wasi__snapshot__preview1 *w, u32 id, u32 out) {
    (void)id;
    put64(w, out, CLOCK_RESOLUTION_NS);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *w, u32 id, u64 precision, u32 out) {
    struct timespec now;

    (void)precision;
    clock_gettime(id == WASI_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
    put64(w, out, (u64)now.tv_sec * NS_PER_SECOND + (u64)now.tv_nsec);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_get(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 out) {
    if (fd != PREOPEN_FD) {
        return WASI_EBADF;
    } else {
        /* The preopened root */
    }
    put32(w, out, WASI_PREOPENTYPE_DIR);
    put32(w, out + PRESTAT_NAME_LENGTH_OFFSET, PREOPEN_NAME_LENGTH);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_prestat_dir_name(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 path, u32 length) {
    if (fd != PREOPEN_FD || length < PREOPEN_NAME_LENGTH) {
        return WASI_EBADF;
    } else {
        /* The preopened root, with room for its name */
    }
    mem(w)[path] = '/';
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_path_open(struct w2c_wasi__snapshot__preview1 *w, u32 dirfd, u32 dirflags, u32 path,
                                           u32 length, u32 oflags, u64 rights, u64 inheriting, u32 fdflags, u32 out) {
    char name[PATH_BYTES];
    int flags = 0;
    int fd;
    int slot;
    int readable = (rights & RIGHT_FD_READ) != 0;
    int writable = (rights & RIGHT_FD_WRITE) != 0;

    (void)dirflags;
    (void)inheriting;
    if (dirfd != PREOPEN_FD) {
        return WASI_EBADF;
    } else {
        /* Under the preopened root */
    }
    host_path(w, path, length, name);
    if (readable && writable) {
        flags = O_RDWR;
    } else if (writable) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }
    if (oflags & OFLAG_CREAT) {
        flags |= O_CREAT;
    } else {
        /* Existing files only */
    }
    if (oflags & OFLAG_EXCL) {
        flags |= O_EXCL;
    } else {
        /* An existing file will do */
    }
    if (oflags & OFLAG_TRUNC) {
        flags |= O_TRUNC;
    } else {
        /* Contents kept */
    }
    if (oflags & OFLAG_DIRECTORY) {
        flags |= O_DIRECTORY;
    } else {
        /* Any file type */
    }
    if (fdflags & FDFLAG_APPEND) {
        flags |= O_APPEND;
    } else {
        /* Writes at the file position */
    }
    fd = open(name, flags, CREATED_FILE_MODE);
    if (fd < 0) {
        return errno_to_wasi(errno);
    } else {
        /* Opened */
    }
    for (slot = PREOPEN_FD + 1; slot < FDS_MAX && w->host_fd[slot] >= 0; slot++) {
        /* The first free slot after the preopened root */
    }
    if (slot == FDS_MAX) {
        close(fd);
        return WASI_EIO;
    } else {
        /* A slot is free */
    }
    w->host_fd[slot] = fd;
    w->is_directory[slot] = (oflags & OFLAG_DIRECTORY) != 0;
    put32(w, out, slot);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, u32 fd) {
    int h = host(w, fd);

    if (h < 0) {
        return WASI_EBADF;
    } else if (fd <= PREOPEN_FD) {
        /* The standard streams and the preopened root stay open */
    } else {
        close(h);
        w->host_fd[fd] = -1;
    }
    return WASI_ESUCCESS;
}

static u32 transfer(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iovs, u32 count, u32 out, int writing,
                    int64_t offset) {
    int h = host(w, fd);
    u32 total = 0;

    if (h < 0) {
        return WASI_EBADF;
    } else {
        /* An open file */
    }
    for (u32 i = 0; i < count; i++) {
        u32 buffer = get32(w, iovs + IOVEC_BYTES * i);
        u32 length = get32(w, iovs + IOVEC_BYTES * i + IOVEC_LENGTH_OFFSET);
        ssize_t done;

        if (writing && offset < 0) {
            done = write(h, mem(w) + buffer, length);
        } else if (writing) {
            done = pwrite(h, mem(w) + buffer, length, offset + total);
        } else if (offset < 0) {
            done = read(h, mem(w) + buffer, length);
        } else {
            done = pread(h, mem(w) + buffer, length, offset + total);
        }
        if (done < 0) {
            return errno_to_wasi(errno);
        } else {
            total += (u32)done;
        }
        if ((u32)done < length) {
            break;
        } else {
            /* The whole buffer: on to the next */
        }
    }
    put32(w, out, total);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iovs, u32 count, u32 out) {
    return transfer(w, fd, iovs, count, out, 0, -1);
}
u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iovs, u32 count, u32 out) {
    return transfer(w, fd, iovs, count, out, 1, -1);
}
u32 w2c_wasi__snapshot__preview1_fd_pread(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iovs, u32 count, u64 offset, u32 out) {
    return transfer(w, fd, iovs, count, out, 0, (int64_t)offset);
}
u32 w2c_wasi__snapshot__preview1_fd_pwrite(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iovs, u32 count, u64 offset, u32 out) {
    return transfer(w, fd, iovs, count, out, 1, (int64_t)offset);
}

u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u64 offset, u32 whence, u32 out) {
    int h = host(w, fd);
    off_t at;

    if (h < 0) {
        return WASI_EBADF;
    } else {
        /* An open file */
    }
    at = lseek(h, (off_t)(int64_t)offset, seek_whence(whence));
    if (at < 0) {
        return errno_to_wasi(errno);
    } else {
        /* Moved */
    }
    put64(w, out, (u64)at);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_tell(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 out) {
    return w2c_wasi__snapshot__preview1_fd_seek(w, fd, 0, WASI_WHENCE_CUR, out);
}

static void filestat(struct w2c_wasi__snapshot__preview1 *w, const struct stat *s, u32 out) {
    memset(mem(w) + out, 0, FILESTAT_BYTES);
    put64(w, out, (u64)s->st_dev);
    put64(w, out + FILESTAT_INO_OFFSET, (u64)s->st_ino);
    mem(w)[out + FILESTAT_FILETYPE_OFFSET] = filetype(s->st_mode);
    put64(w, out + FILESTAT_NLINK_OFFSET, (u64)s->st_nlink);
    put64(w, out + FILESTAT_SIZE_OFFSET, (u64)s->st_size);
}

u32 w2c_wasi__snapshot__preview1_fd_fdstat_get(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 out) {
    int h = host(w, fd);
    struct stat s;

    if (h < 0) {
        return WASI_EBADF;
    } else {
        /* An open file */
    }
    memset(mem(w) + out, 0, FDSTAT_BYTES);
    if (fstat(h, &s) != 0) {
        return errno_to_wasi(errno);
    } else {
        /* Its type known */
    }
    mem(w)[out] = filetype(s.st_mode);
    put64(w, out + FDSTAT_RIGHTS_BASE_OFFSET, ALL_RIGHTS);
    put64(w, out + FDSTAT_RIGHTS_INHERITING_OFFSET, ALL_RIGHTS);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_fd_filestat_get(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 out) {
    int h = host(w, fd);
    struct stat s;

    if (h < 0) {
        return WASI_EBADF;
    } else {
        /* An open file */
    }
    if (fstat(h, &s) != 0) {
        return errno_to_wasi(errno);
    } else {
        /* Its status known */
    }
    filestat(w, &s, out);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_path_filestat_get(struct w2c_wasi__snapshot__preview1 *w, u32 dirfd, u32 flags, u32 path, u32 length, u32 out) {
    char name[PATH_BYTES];
    struct stat s;

    (void)flags;
    if (dirfd != PREOPEN_FD) {
        return WASI_EBADF;
    } else {
        /* Under the preopened root */
    }
    host_path(w, path, length, name);
    if (stat(name, &s) != 0) {
        return errno_to_wasi(errno);
    } else {
        /* Its status known */
    }
    filestat(w, &s, out);
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_path_create_directory(struct w2c_wasi__snapshot__preview1 *w, u32 dirfd, u32 path, u32 length) {
    char name[PATH_BYTES];

    if (dirfd != PREOPEN_FD) {
        return WASI_EBADF;
    } else {
        /* Under the preopened root */
    }
    host_path(w, path, length, name);
    return mkdir(name, CREATED_DIRECTORY_MODE) == 0 ? WASI_ESUCCESS : errno_to_wasi(errno);
}

void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *w, u32 code) {
    (void)w;
    exit((int)code);
}

u32 w2c_wasi__snapshot__preview1_random_get(struct w2c_wasi__snapshot__preview1 *w, u32 buffer, u32 length) {
    for (u32 i = 0; i < length; i++) {
        mem(w)[buffer + i] = (uint8_t)rand();
    }
    return WASI_ESUCCESS;
}

u32 w2c_wasi__snapshot__preview1_sched_yield(struct w2c_wasi__snapshot__preview1 *w) { (void)w; return WASI_ESUCCESS; }
u32 w2c_wasi__snapshot__preview1_poll_oneoff(struct w2c_wasi__snapshot__preview1 *w, u32 a, u32 b, u32 c, u32 out) {
    (void)a;
    (void)b;
    (void)c;
    put32(w, out, 0);
    return WASI_ESUCCESS;
}
u32 w2c_wasi__snapshot__preview1_fd_sync(struct w2c_wasi__snapshot__preview1 *w, u32 fd) { (void)w; (void)fd; return WASI_ESUCCESS; }
u32 w2c_wasi__snapshot__preview1_fd_datasync(struct w2c_wasi__snapshot__preview1 *w, u32 fd) { (void)w; (void)fd; return WASI_ESUCCESS; }
u32 w2c_wasi__snapshot__preview1_fd_fdstat_set_flags(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 f) { (void)w; (void)fd; (void)f; return WASI_ESUCCESS; }

/* Not used headless */
#define NOSYS(name, ...) u32 w2c_wasi__snapshot__preview1_##name(struct w2c_wasi__snapshot__preview1 *w, __VA_ARGS__) { (void)w; return WASI_ENOSYS; }
NOSYS(fd_advise, u32 a, u64 b, u64 c, u32 d)
NOSYS(fd_allocate, u32 a, u64 b, u64 c)
NOSYS(fd_fdstat_set_rights, u32 a, u64 b, u64 c)
NOSYS(fd_filestat_set_size, u32 a, u64 b)
NOSYS(fd_filestat_set_times, u32 a, u64 b, u64 c, u32 d)
NOSYS(fd_readdir, u32 a, u32 b, u32 c, u64 d, u32 e)
NOSYS(fd_renumber, u32 a, u32 b)
NOSYS(path_filestat_set_times, u32 a, u32 b, u32 c, u32 d, u64 e, u64 f, u32 g)
NOSYS(path_link, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g)
NOSYS(path_readlink, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f)
NOSYS(path_remove_directory, u32 a, u32 b, u32 c)
NOSYS(path_rename, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f)
NOSYS(path_symlink, u32 a, u32 b, u32 c, u32 d, u32 e)
NOSYS(path_unlink_file, u32 a, u32 b, u32 c)
NOSYS(sock_accept, u32 a, u32 b, u32 c)
NOSYS(sock_recv, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f)
NOSYS(sock_send, u32 a, u32 b, u32 c, u32 d, u32 e)
NOSYS(sock_shutdown, u32 a, u32 b)

int main(int argc, char **argv) {
    static w2c_crash2 module;
    static struct w2c_wasi__snapshot__preview1 wasi;
    wasm_rt_trap_t trap;

    wasi.module = &module;
    wasi.argc = argc;
    wasi.argv = argv;
    for (int i = 0; i < FDS_MAX; i++) {
        wasi.host_fd[i] = -1;
    }
    wasi.host_fd[STDIN_FILENO] = STDIN_FILENO;
    wasi.host_fd[STDOUT_FILENO] = STDOUT_FILENO;
    wasi.host_fd[STDERR_FILENO] = STDERR_FILENO;
    wasi.host_fd[PREOPEN_FD] = open("/", O_RDONLY | O_DIRECTORY);
    wasm_rt_init();
    wasm2c_crash2_instantiate(&module, &wasi);
    trap = wasm_rt_impl_try();
    if (trap == WASM_RT_TRAP_NONE) {
        w2c_crash2_0x5Fstart(&module);
    } else {
        fprintf(stderr, "spike: trap %s\n", wasm_rt_strerror(trap));
        return EXIT_FAILURE;
    }
    wasm2c_crash2_free(&module);
    wasm_rt_free();
    return 0;
}
