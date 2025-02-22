// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <gnu/lib-names.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/fcntl.h>
#include <sys/xattr.h>

#include "bxl_observer.hpp"
#include "observer_utilities.hpp"
#include "PTraceSandbox.hpp"

#define ERROR_RETURN_VALUE -1

static std::string sEmptyStr("");

// Propagates errno when the result is -1.
static int get_errno_from_result(result_t<int> result)
{
    return result.get() == -1 ? result.get_errno() : 0;
}

INTERPOSE(int, statx, int dirfd, const char * pathname, int flags, unsigned int mask, struct statx * statxbuf)({
    AccessReportGroup report;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_STAT, dirfd, pathname, report);
    return bxl->check_fwd_and_report_statx(report, check, ERROR_RETURN_VALUE, dirfd, pathname, flags, mask, statxbuf);
})


INTERPOSE(int, scandir, const char * dirp,
                   struct dirent *** namelist,
                   int (*filter)(const struct dirent *),
                   int (*compar)(const struct dirent **, const struct dirent **))
({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, dirp, report);
    return bxl->check_fwd_and_report_scandir(report, check, ERROR_RETURN_VALUE, dirp, namelist, filter, compar);
})

INTERPOSE(int, scandir64, const char * dirp,
                   struct dirent64 *** namelist,
                   int (*filter)(const struct dirent64  *),
                   int (*compar)(const dirent64 **, const dirent64 **))
({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, dirp, report);
    return bxl->check_fwd_and_report_scandir64(report, check, ERROR_RETURN_VALUE, dirp, namelist, filter, compar);
})

INTERPOSE(int, scandirat, int dirfd, const char * dirp,
                   struct dirent *** namelist,
                   int (*filter)(const struct dirent *),
                   int (*compar)(const struct dirent **, const struct dirent **))
({
    AccessReportGroup report;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, dirfd, dirp, report);
    return bxl->check_fwd_and_report_scandirat(report, check, ERROR_RETURN_VALUE, dirfd, dirp, namelist, filter, compar);
})

INTERPOSE(int, scandirat64, int dirfd, const char * dirp,
                   struct dirent64 *** namelist,
                   int (*filter)(const struct dirent64  *),
                   int (*compar)(const dirent64 **, const dirent64 **))
({
    AccessReportGroup report;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, dirfd, dirp, report);
    return bxl->check_fwd_and_report_scandirat64(report, check, ERROR_RETURN_VALUE, dirfd, dirp, namelist, filter, compar);
})

INTERPOSE(struct dirent *, readdir, DIR *dirp)
({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, dirfd(dirp), report);
    return bxl->check_fwd_and_report_readdir(report, check, (struct dirent *)NULL, dirp);
})

INTERPOSE(struct dirent64 *, readdir64, DIR *dirp)
({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, dirfd(dirp), report);
    return bxl->check_fwd_and_report_readdir64(report, check, (struct dirent64 *)NULL, dirp);
})

INTERPOSE(int, readdir_r, DIR *dirp, struct dirent *entry, struct dirent **result)
({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, dirfd(dirp), report);
    return bxl->check_fwd_and_report_readdir_r(report, check, ERROR_RETURN_VALUE, dirp, entry, result);
})

INTERPOSE(int, readdir64_r, DIR *dirp, struct dirent64 *entry, struct dirent64 **result)
({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, dirfd(dirp), report);
    return bxl->check_fwd_and_report_readdir64_r(report, check, ERROR_RETURN_VALUE, dirp, entry, result);
})

INTERPOSE(void, _exit, int status)({
    char emptystr[1] = {'\0'};
    bxl->report_access("_exit", ES_EVENT_TYPE_NOTIFY_EXIT, emptystr, emptystr);
    bxl->real__exit(status);
    _exit(status);
})

static void report_child_process(const char *syscall, BxlObserver *bxl, pid_t childPid, pid_t parentPid)
{
    string exePath(bxl->GetProgramPath());
    // Events of type ES_EVENT_TYPE_NOTIFY_FORK are expected to contain the spawned child process id in its cpid field (the parent pid 
    // is actually ignored and not sent as part of the report, we pass it here for consistency only).
    IOEvent event(parentPid, childPid, 0, ES_EVENT_TYPE_NOTIFY_FORK, ES_ACTION_TYPE_NOTIFY, exePath, std::string(""), exePath, 0, false);
    bxl->report_access(syscall, event);
}

static int ret_fd(int fd, BxlObserver *bxl) 
{
    // when returning a new file descriptor we remove it from our cache, 
    // because presumably the path has changed
    bxl->reset_fd_table_entry(fd);
    return fd;
}

INTERPOSE(pid_t, fork, void)({
    result_t<pid_t> childPid = bxl->fwd_fork();

    if (childPid.get() == 0)
    {
        // Clear the file descriptor table when we are in the child process
        // File descriptors are unique to a process, so this cache needs to be invalidated on the child
        bxl->reset_fd_table();
        // Process creation is reported on the child to guarantee that we see the process creation arriving as a report line before
        // any other access report coming from the child
        report_child_process(__func__, bxl, getpid(), getppid());
    }

    return childPid.restore();
})

INTERPOSE(int, clone, int (*fn)(void *), void *child_stack, int flags, void *arg, ... /* pid_t *ptid, void *newtls, pid_t *ctid */ )({
    va_list args;
    va_start(args, arg);
    pid_t *ptid = va_arg(args, pid_t*);
    void *newtls = va_arg(args, void*);
    pid_t *ctid = va_arg(args, pid_t*);
    va_end(args);

    result_t<int> result = bxl->fwd_clone(fn, child_stack, flags, arg, ptid, newtls, ctid);
    
    if (result.get() == 0)
    {
        // Clear the file descriptor table when we are in the child process
        // File descriptors are unique to a process, so this cache needs to be invalidated on the child
        bxl->reset_fd_table();
        // Process creation is reported on the child to guarantee that we see the process creation arriving as a report line before
        // any other access report coming from the child
        report_child_process(__func__, bxl, getpid(), getppid());
    }

    return result.restore();
})

static int handle_exec_with_ptrace(const char *file, char *const argv[], char *const envp[], BxlObserver *bxl)
{
    // fdtable will not longer be valid because the process will be forked for ptrace
    bxl->reset_fd_table();
    
    // Before we enable the ptrace sandbox, make sure we disable the interposed sandbox
    // This shouldn't make a difference for real builds (we are enabling the ptrace sandbox because
    // we are about to run a statically linked process, and therefore libc is not there) but for tests
    // we may use the ptrace sandbox even for dynamically linked processes.
    envp = bxl->RemoveLDPreloadFromEnv(envp);

    PTraceSandbox ptraceSandbox(bxl);
    auto result = ptraceSandbox.ExecuteWithPTraceSandbox(file, argv, envp, bxl->getPTraceMqName(), bxl->getFamPath());

    bxl->report_exec("execve", argv[0], file, /* error */ errno);

    return result;
}

static int handle_exec_with_ptrace(int fd, char *const argv[], char *const envp[], BxlObserver *bxl)
{
    auto resolvedPath = bxl->fd_to_path(fd).c_str();
    handle_exec_with_ptrace(resolvedPath, argv, envp, bxl);
}

INTERPOSE(int, fexecve, int fd, char *const argv[], char *const envp[])({
    // exec* functions start a new instance of the sandbox and therefore the process creation report
    // is sent on __init__

    if (bxl->check_and_report_statically_linked_process(fd))
    {
        return handle_exec_with_ptrace(fd, argv, bxl->ensureEnvs(envp), bxl);
    }

    result_t<int> result = bxl->fwd_fexecve(fd, argv, bxl->ensureEnvs(envp));

    // This will only execute if exec failed
    bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_EXEC, fd, result.get_errno());

    return result.restore();
})

INTERPOSE(int, execv, const char *file, char *const argv[])({
    // exec* functions start a new instance of the sandbox and therefore the process creation report
    // is sent on __init__

    if (bxl->check_and_report_statically_linked_process(file))
    {
        return handle_exec_with_ptrace(file, argv, bxl->ensureEnvs(environ), bxl);
    }

    result_t<int> result =  bxl->fwd_execve(file, argv, bxl->ensureEnvs(environ));

    // This will only execute if exec failed
    bxl->report_exec(__func__, argv[0], file,  result.get_errno());

    return result.restore();
})

INTERPOSE(int, execve, const char *file, char *const argv[], char *const envp[])({
    // exec* functions start a new instance of the sandbox and therefore the process creation report
    // is sent on __init__

    if (bxl->check_and_report_statically_linked_process(file))
    {
        return handle_exec_with_ptrace(file, argv, bxl->ensureEnvs(envp), bxl);
    }

    result_t<int> result =  bxl->fwd_execve(file, argv, bxl->ensureEnvs(envp));

    // This will only execute if exec failed
    bxl->report_exec(__func__, argv[0], file,  result.get_errno());

    return result.restore();
})

INTERPOSE(int, execvp, const char *file, char *const argv[])({
    // exec* functions start a new instance of the sandbox and therefore the process creation report
    // is sent on __init__

    mode_t mode = 0;
    std::string pathname;
    auto path_resolution_result = resolve_filename_with_env(file, mode, pathname);

    if (path_resolution_result)
    {
        if (bxl->check_and_report_statically_linked_process(pathname.c_str()))
        {
            return handle_exec_with_ptrace(pathname.c_str(), argv, bxl->ensureEnvs(environ), bxl);
        }

        result_t<int> result = bxl->fwd_execve(pathname.c_str(), argv, bxl->ensureEnvs(environ));
        bxl->report_exec(__func__, argv[0], pathname.c_str(), /*error*/ result.get_errno(), mode);
        return result.restore();
    }
    else
    {
        // exec* functions don't return unless they fail (the executing image gets replaced
        // by the specified one). So we cannot actually report back the errno
        result_t<int> result = bxl->fwd_execvpe(file, argv, bxl->ensureEnvs(environ));
        bxl->report_exec(__func__, argv[0], file,  /* error */ result.get_errno(), mode);
        return result.restore();
    }
})

INTERPOSE(int, execvpe, const char *file, char *const argv[], char *const envp[])({
    // exec* functions start a new instance of the sandbox and therefore the process creation report
    // is sent on __init__

    mode_t mode = 0;
    std::string pathname;
    auto path_resolution_result = resolve_filename_with_env(file, mode, pathname);

    // If the path couldn't be resolved, then the exec will likely fail anyways
    if (path_resolution_result)
    {
        if (bxl->check_and_report_statically_linked_process(pathname.c_str()))
        {
            return handle_exec_with_ptrace(pathname.c_str(), argv, bxl->ensureEnvs(envp), bxl);
        }

        result_t<int> result = bxl->fwd_execve(pathname.c_str(), argv, bxl->ensureEnvs(envp));
        // This will only execute if exec failed
        bxl->report_exec(__func__, argv[0], pathname.c_str(), /*error*/ result.get_errno(), mode);
        return result.restore();
    }
    else
    {
        result_t<int> result = bxl->fwd_execve(file, argv, bxl->ensureEnvs(envp));
        // This will only execute if exec failed
        bxl->report_exec(__func__, argv[0], file, /* error */ result.get_errno(), mode);
        return result.restore();
    }
})

INTERPOSE(int, execl, const char *pathname, const char *arg, ...)({
    va_list args;
    va_start(args, arg);
    ptrdiff_t argc = get_variadic_argc(args);
    va_end(args);

    if (argc == -1)
    {
        return -1;
    }

    va_start(args, arg);
    char *argv[argc + 1];
    parse_variadic_args(arg, argc, args, argv);
    va_end(args);

    if (bxl->check_and_report_statically_linked_process(pathname))
    {
        return handle_exec_with_ptrace(pathname, (char **)argv, bxl->ensureEnvs(environ), bxl);
    }
    
    result_t<int> result = bxl->fwd_execve(pathname, (char **)argv, bxl->ensureEnvs(environ));
    bxl->report_exec(__func__, argv[0], pathname, /*error*/ result.get_errno(), /*mode*/ 0);
    return result.restore();
})

INTERPOSE(int, execlp, const char *file, const char *arg, ...)({
    va_list args;
    va_start(args, arg);
    ptrdiff_t argc = get_variadic_argc(args);
    va_end(args);

    if (argc == -1)
    {
        return -1;
    }

    va_start(args, arg);
    char *argv[argc + 1];
    parse_variadic_args(arg, argc, args, argv);
    va_end(args);

    mode_t mode = 0;
    std::string pathname;
    auto path_resolution_result = resolve_filename_with_env(file, mode, pathname);

    if (path_resolution_result)
    {
        if (bxl->check_and_report_statically_linked_process(pathname.c_str()))
        {
            return handle_exec_with_ptrace(pathname.c_str(), (char **)argv, bxl->ensureEnvs(environ), bxl);
        }

        result_t<int> result = bxl->fwd_execve(pathname.c_str(), (char **)argv, bxl->ensureEnvs(environ));
        bxl->report_exec(__func__, argv[0], pathname.c_str(), /*error*/ result.get_errno(), mode);
        return result.restore();
    }
    else
    {
        result_t<int> result = bxl->fwd_execvp(file, (char **)argv);
        bxl->report_exec(__func__, argv[0], file, /*error*/ result.get_errno(), mode);
        return result.restore();
    }
})

INTERPOSE(int, execle, const char *pathname, const char *arg, ...)({
    va_list args;
    va_start(args, arg);
    ptrdiff_t argc = get_variadic_argc(args);
    va_end(args);

    if (argc == -1)
    {
        return -1;
    }

    va_start(args, arg);
    char *argv[argc + 1];
    char **envp;
    parse_variadic_args(arg, argc, args, argv);
    envp = va_arg(args, char **);
    va_end(args);

    if (bxl->check_and_report_statically_linked_process(pathname))
    {
        return handle_exec_with_ptrace(pathname, (char **)argv, bxl->ensureEnvs(envp), bxl);
    }

    result_t<int> result = bxl->fwd_execve(pathname, (char **)argv, bxl->ensureEnvs(envp));

    // This will only execute if exec failed
    bxl->report_exec(__func__, argv[0], pathname, result.get_errno(), /* mode */ 0);

    return result.restore();
})

#if (__GLIBC__ == 2 && __GLIBC_MINOR__ < 33)
INTERPOSE(int, __fxstat, int __ver, int fd, struct stat *__stat_buf)({
    result_t<int> result = bxl->fwd___fxstat(__ver, fd, __stat_buf);
    bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, __fxstat64, int __ver, int fd, struct stat64 *buf)({
    result_t<int> result(bxl->fwd___fxstat64(__ver, fd, buf));
    bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, __fxstatat, int __ver, int fd, const char *pathname, struct stat *__stat_buf, int flag)({
    result_t<int> result = bxl->fwd___fxstatat(__ver, fd, pathname, __stat_buf, flag);
    bxl->report_access_at(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd, pathname, 0, true, /* associated_pid */ 0, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, __fxstatat64, int __ver, int fd, const char *pathname, struct stat64 *buf, int flag)({
    result_t<int> result = bxl->fwd___fxstatat64(__ver, fd, pathname, buf, flag);
    bxl->report_access_at(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd, pathname, 0, true, /* associated_pid */ 0, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, __xstat, int __ver, const char *pathname, struct stat *buf)({
    result_t<int> result = bxl->fwd___xstat(__ver, pathname, buf);
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname, /* mode */0, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, __xstat64, int __ver, const char *pathname, struct stat64 *buf)({
    result_t<int> result(bxl->fwd___xstat64(__ver, pathname, buf));
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname, /* mode */0, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, __lxstat, int __ver, const char *pathname, struct stat *buf)({
    result_t<int> result = bxl->fwd___lxstat(__ver, pathname, buf);
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname, /* mode */0, O_NOFOLLOW, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, __lxstat64, int __ver, const char *pathname, struct stat64 *buf)({
    result_t<int> result(bxl->fwd___lxstat64(__ver, pathname, buf));
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname, /* mode */0, O_NOFOLLOW, get_errno_from_result(result));
    return result.restore();
})
#else
INTERPOSE(int, stat, const char *pathname, struct stat *statbuf)({
    result_t<int> result = bxl->fwd_stat(pathname, statbuf);
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname, /* mode */0, O_NOFOLLOW, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, stat64, const char *pathname, struct stat64 *statbuf)({
    result_t<int> result = bxl->fwd_stat64(pathname, statbuf);
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname, /* mode */0, O_NOFOLLOW, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, lstat, const char *pathname, struct stat *statbuf)({
    result_t<int> result = bxl->fwd_lstat(pathname, statbuf);
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname, /* mode */0, O_NOFOLLOW, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, lstat64, const char *pathname, struct stat64 *statbuf)({
    result_t<int> result = bxl->fwd_lstat64(pathname, statbuf);
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname, /* mode */0, O_NOFOLLOW, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, fstat, int fd, struct stat *statbuf)({
    result_t<int> result = bxl->fwd_fstat(fd, statbuf);
    bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd, get_errno_from_result(result));
    return result.restore();
})

INTERPOSE(int, fstat64, int fd, struct stat64 *statbuf)({
    result_t<int> result = bxl->fwd_fstat64(fd, statbuf);
    bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd, get_errno_from_result(result));
    return result.restore();
})
#endif

static es_event_type_t get_event_from_open_mode(const char *mode) {
    const char *pMode = mode;
    while (pMode && *pMode) {
        if (*pMode == 'a' || *pMode == 'w' || *pMode == '+') {
            return ES_EVENT_TYPE_NOTIFY_WRITE;
        }
        ++pMode;
    }
    return ES_EVENT_TYPE_NOTIFY_OPEN;
}

INTERPOSE(FILE*, fdopen, int fd, const char *mode)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, get_event_from_open_mode(mode), fd, report);
    return bxl->check_fwd_and_report_fdopen(report, check, (FILE*)NULL, fd, mode);
})

INTERPOSE(FILE*, fopen, const char *pathname, const char *mode)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, get_event_from_open_mode(mode), pathname, report);
    FILE *f = bxl->check_fwd_and_report_fopen(report, check, (FILE*)NULL, pathname, mode);
    if (f) { bxl->reset_fd_table_entry(fileno(f)); }
    return f;
})

INTERPOSE(FILE*, fopen64, const char *pathname, const char *mode)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, get_event_from_open_mode(mode), pathname, report);
    FILE *f = bxl->check_fwd_and_report_fopen64(report, check, (FILE*)NULL, pathname, mode);
    if (f) { bxl->reset_fd_table_entry(fileno(f)); }
    return f;
})

INTERPOSE(FILE*, freopen, const char *pathname, const char *mode, FILE *stream)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, get_event_from_open_mode(mode), pathname, report);
    FILE *f = bxl->check_fwd_and_report_freopen(report, check, (FILE*)NULL, pathname, mode, stream);
    if (f) { bxl->reset_fd_table_entry(fileno(f)); }
    return f;
})

INTERPOSE(FILE*, freopen64, const char *pathname, const char *mode, FILE *stream)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, get_event_from_open_mode(mode), pathname, report);
    FILE *f = bxl->check_fwd_and_report_freopen64(report, check, (FILE*)NULL, pathname, mode, stream);
    if (f) { bxl->reset_fd_table_entry(fileno(f)); }
    return f;
})

INTERPOSE(size_t, fread, void *ptr, size_t size, size_t nmemb, FILE *stream)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_OPEN, fileno(stream), report);
    return bxl->check_fwd_and_report_fread(report, check, (size_t)0, ptr, size, nmemb, stream);
})

INTERPOSE(size_t, fwrite, const void *ptr, size_t size, size_t nmemb, FILE *stream)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fileno(stream), report);
    return bxl->check_fwd_and_report_fwrite(report, check, (size_t)0, ptr, size, nmemb, stream);
})

INTERPOSE(int, fputc, int c, FILE *stream)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fileno(stream), report);
    return bxl->check_fwd_and_report_fputc(report, check, ERROR_RETURN_VALUE, c, stream);
})

INTERPOSE(int, fputs, const char *s, FILE *stream)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fileno(stream), report);
    return bxl->check_fwd_and_report_fputs(report, check, ERROR_RETURN_VALUE, s, stream);
})

INTERPOSE(int, putc, int c, FILE *stream)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fileno(stream), report);
    return bxl->check_fwd_and_report_putc(report, check, ERROR_RETURN_VALUE, c, stream);
})

INTERPOSE(int, putchar, int c)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fileno(stdout), report);
    return bxl->check_fwd_and_report_putchar(report, check, ERROR_RETURN_VALUE, c);
})

INTERPOSE(int, puts, const char *s)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fileno(stdout), report);
    return bxl->check_fwd_and_report_puts(report, check, ERROR_RETURN_VALUE, s);
})

INTERPOSE(int, access, const char *pathname, int mode)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_ACCESS, pathname, report);
    return bxl->check_fwd_and_report_access(report, check, ERROR_RETURN_VALUE, pathname, mode);
})

INTERPOSE(int, faccessat, int dirfd, const char *pathname, int mode, int flags)({
    AccessReportGroup report;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_ACCESS, dirfd, pathname, report);
    return bxl->check_fwd_and_report_faccessat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, mode, flags);
})

// report "Create" if path does not exist and O_CREAT or O_TRUNC is specified
// report "Write" if path exists and O_CREAT or O_TRUNC is specified (because this truncates the file regardless of its content)
// otherwise, report "Read"
static AccessCheckResult CreateFileOpen(BxlObserver *bxl, string &pathStr, int oflag, AccessReportGroup &report)
{
    mode_t pathMode = bxl->get_mode(pathStr.c_str());
    bool pathExists = pathMode != 0;
    bool isCreate = !pathExists && (oflag & (O_CREAT|O_TRUNC));
    bool hasWriteAccess = ((oflag & O_ACCMODE) == O_WRONLY) || ((oflag & O_ACCMODE) == O_RDWR);
    bool isWrite = pathExists && (oflag & (O_CREAT|O_TRUNC) && hasWriteAccess);
    IOEvent event(
        isCreate ? ES_EVENT_TYPE_NOTIFY_CREATE : isWrite ? ES_EVENT_TYPE_NOTIFY_WRITE : ES_EVENT_TYPE_NOTIFY_OPEN,
        ES_ACTION_TYPE_NOTIFY,
        pathStr, bxl->GetProgramPath(), pathMode, false, "");
    return bxl->create_access(__func__, event, report);
}

INTERPOSE(int, open, const char *path, int oflag, ...)({
    va_list args;
    va_start(args, oflag);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);

    std::string pathStr = bxl->normalize_path(path);
    AccessReportGroup report;
    AccessCheckResult check = CreateFileOpen(bxl, pathStr, oflag, report);
    return ret_fd(bxl->check_fwd_and_report_open(report, check, ERROR_RETURN_VALUE, path, oflag, mode), bxl);
})

INTERPOSE(int, open64, const char *path, int oflag, ...)({
    va_list args;
    va_start(args, oflag);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);

    std::string pathStr = bxl->normalize_path(path);
    AccessReportGroup report;
    AccessCheckResult check = CreateFileOpen(bxl, pathStr, oflag, report);
    return ret_fd(bxl->check_fwd_and_report_open64(report, check, ERROR_RETURN_VALUE, path, oflag, mode), bxl);
})

INTERPOSE(int, openat, int dirfd, const char *pathname, int flags, ...)({
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);

    std::string pathStr = bxl->normalize_path_at(dirfd, pathname);
    AccessReportGroup report;
    AccessCheckResult check = CreateFileOpen(bxl, pathStr, flags, report);
    return ret_fd(bxl->check_fwd_and_report_openat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, flags, mode), bxl);
})

INTERPOSE(int, openat64, int dirfd, const char *pathname, int flags, ...)({
    va_list args;
    va_start(args, flags);
    mode_t mode = va_arg(args, mode_t);
    va_end(args);

    std::string pathStr = bxl->normalize_path_at(dirfd, pathname);
    AccessReportGroup report;
    AccessCheckResult check = CreateFileOpen(bxl, pathStr, flags, report);
    return ret_fd(bxl->check_fwd_and_report_openat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, flags, mode), bxl);
})

INTERPOSE(int, creat, const char *pathname, mode_t mode)({
    return open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
})

INTERPOSE(ssize_t, write, int fd, const void *buf, size_t bufsiz)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd, report);
    return bxl->check_fwd_and_report_write(report, check, (ssize_t)ERROR_RETURN_VALUE, fd, buf, bufsiz);
})

INTERPOSE(ssize_t, pwrite, int fd, const void *buf, size_t count, off_t offset)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd, report);
    return bxl->check_fwd_and_report_pwrite(report, check, (ssize_t)ERROR_RETURN_VALUE, fd, buf, count, offset);
})

INTERPOSE(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd, report);
    return bxl->check_fwd_and_report_writev(report, check, (ssize_t)ERROR_RETURN_VALUE, fd, iov, iovcnt);
})

INTERPOSE(ssize_t, pwritev, int fd, const struct iovec *iov, int iovcnt, off_t offset)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd, report);
    return bxl->check_fwd_and_report_pwritev(report, check, (ssize_t)ERROR_RETURN_VALUE, fd, iov, iovcnt, offset);
})

INTERPOSE(ssize_t, pwritev2, int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd, report);
    return bxl->check_fwd_and_report_pwritev2(report, check, (ssize_t)ERROR_RETURN_VALUE, fd, iov, iovcnt, offset, flags);
})

INTERPOSE(ssize_t, pwrite64, int fd, const void *buf, size_t count, off_t offset)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd, report);
    return bxl->check_fwd_and_report_pwrite64(report, check, (ssize_t)ERROR_RETURN_VALUE, fd, buf, count, offset);
})

INTERPOSE(int, remove, const char *pathname)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_UNLINK, pathname, report, /*mode*/0, O_NOFOLLOW);
    return bxl->check_fwd_and_report_remove(report, check, ERROR_RETURN_VALUE, pathname);
})

INTERPOSE(int, truncate, const char *path, off_t length)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, path, report);
    return bxl->check_fwd_and_report_truncate(report, check, (ssize_t)ERROR_RETURN_VALUE, path, length);
})

INTERPOSE(int, ftruncate, int fd, off_t length)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd, report);
    return bxl->check_fwd_and_report_ftruncate(report, check, (ssize_t)ERROR_RETURN_VALUE, fd, length);
})

INTERPOSE(int, truncate64, const char *path, off_t length)({
    return truncate(path, length);
})

INTERPOSE(int, ftruncate64, int fd, off_t length)({
    return ftruncate(fd, length);
})

INTERPOSE(int, rmdir, const char *pathname)({
    AccessReportGroup report;
    // We need to know all the rmdir attempts so we can identify which failed/succeeded, so don't use the cache
    // This is so we can track directory creation/deletion flow. Using the cache lumps all these operations into one report line
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_UNLINK, pathname, report, /* mode */ 0, /* flags */ 0 , /* checkCache */ false);

    return bxl->check_fwd_and_report_rmdir(report, check, ERROR_RETURN_VALUE, pathname);
})

INTERPOSE(int, renameat, int olddirfd, const char *oldpath, int newdirfd, const char *newpath)({
    string oldStr = bxl->normalize_path_at(olddirfd, oldpath, O_NOFOLLOW);
    string newStr = bxl->normalize_path_at(newdirfd, newpath, O_NOFOLLOW);

    mode_t mode = bxl->get_mode(oldStr.c_str());    
    AccessCheckResult check = AccessCheckResult::Invalid();
    std::vector<std::string> filesAndDirectories;
    std::vector<AccessReportGroup> accessesToReport;

    if (S_ISDIR(mode))
    {
        bool enumerateResult = bxl->EnumerateDirectory(oldStr, /*recursive*/true, filesAndDirectories);
        if (enumerateResult)
        {
            // reserve all the content for both source and destination
            accessesToReport.reserve(filesAndDirectories.size() * 2);

            for (auto fileOrDirectory : filesAndDirectories)
            {
                // Access check for the source file
                AccessReportGroup sourceReport;
                check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_UNLINK, fileOrDirectory.c_str(), sourceReport, /*mode*/ 0, O_NOFOLLOW);
                accessesToReport.emplace_back(sourceReport);

                // Access check for the destination file
                fileOrDirectory.replace(0, oldStr.length(), newStr);
                AccessReportGroup targetReport;
                check = AccessCheckResult::Combine(check, CreateFileOpen(bxl, fileOrDirectory, O_CREAT | O_WRONLY, targetReport));
                accessesToReport.emplace_back(targetReport);

                // If access is denied to any of the files in the enumeration, we can break the loop right away here because check_and_fwd_renameat will also fail
                if (bxl->should_deny(check))
                {
                    break;
                }
            }
        }
        else
        {
            // TODO: [pgunasekara] Remove this case when we're certain the enumeration logic above is solid
            AccessReportGroup report;
            IOEvent event(ES_EVENT_TYPE_NOTIFY_RENAME, ES_ACTION_TYPE_NOTIFY, oldStr, bxl->GetProgramPath(), mode, false, newStr);
            check = bxl->create_access(__func__, event, report);
            accessesToReport.emplace_back(report);
        }
    }
    else
    {
        AccessReportGroup sourceReport;
        check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_UNLINK, oldStr.c_str(), sourceReport, /*mode*/0, O_NOFOLLOW);
        accessesToReport.emplace_back(sourceReport);
        AccessReportGroup destReport;
        check = AccessCheckResult::Combine(check, CreateFileOpen(bxl, newStr, O_CREAT | O_WRONLY, destReport));
        accessesToReport.emplace_back(destReport);
    }

    result_t<int> result(ERROR_RETURN_VALUE, EPERM);;
    
    if (bxl->should_deny(check))
    {
        // It is enough that we send a single report as a witness for the denial
        // The last one in the array, in particular, is what should have triggere the denial
        bxl->SendReport(accessesToReport.back());
    }
    else 
    {
        result = bxl->fwd_renameat(olddirfd, oldpath, newdirfd, newpath);
        for (auto access : accessesToReport)
        {
            access.SetErrno(get_errno_from_result(result));
            bxl->SendReport(access);
        }
    }

    return result.restore();
})

INTERPOSE(int, rename, const char *oldpath, const char *newpath)({ 
    return renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath);
})

INTERPOSE(int, link, const char *path1, const char *path2)({
    AccessReportGroup report;
    auto check = bxl->create_access(
        __func__,
        ES_EVENT_TYPE_NOTIFY_LINK,
        bxl->normalize_path(path1, O_NOFOLLOW).c_str(),
        bxl->normalize_path(path2, O_NOFOLLOW).c_str(),
        report);
    return bxl->check_fwd_and_report_link(report, check, ERROR_RETURN_VALUE, path1, path2);
})

INTERPOSE(int, linkat, int fd1, const char *name1, int fd2, const char *name2, int flag)({
    AccessReportGroup report;
    auto check = bxl->create_access(
        __func__,
        ES_EVENT_TYPE_NOTIFY_LINK,
        bxl->normalize_path_at(fd1, name1, O_NOFOLLOW).c_str(),
        bxl->normalize_path_at(fd2, name2, O_NOFOLLOW).c_str(),
        report);
    return bxl->check_fwd_and_report_linkat(report, check, ERROR_RETURN_VALUE, fd1, name1, fd2, name2, flag);
})

INTERPOSE(int, unlink, const char *path)({
    if (path && *path == '\0')
    {
        return bxl->fwd_unlink(path).restore();
    }
    
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_UNLINK, path, report, /*mode*/ 0, O_NOFOLLOW);
    return bxl->check_fwd_and_report_unlink(report, check, ERROR_RETURN_VALUE, path);
})

INTERPOSE(int, unlinkat, int dirfd, const char *path, int flags)({
    if (dirfd == AT_FDCWD && path && *path == '\0')
    {
        return bxl->fwd_unlinkat(dirfd, path, flags).restore();
    }

    AccessReportGroup report;
    int oflags = (flags & AT_REMOVEDIR) ? 0 : O_NOFOLLOW;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_UNLINK, dirfd, path, report, oflags);
    return bxl->check_fwd_and_report_unlinkat(report, check, ERROR_RETURN_VALUE, dirfd, path, flags);
})

INTERPOSE(int, symlink, const char *target, const char *linkPath)({
    IOEvent event(ES_EVENT_TYPE_NOTIFY_CREATE, ES_ACTION_TYPE_NOTIFY, bxl->normalize_path(linkPath, O_NOFOLLOW), bxl->GetProgramPath(), S_IFLNK);
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, event, report);
    return bxl->check_fwd_and_report_symlink(report, check, ERROR_RETURN_VALUE, target, linkPath);
})

INTERPOSE(int, symlinkat, const char *target, int dirfd, const char *linkPath)({
    IOEvent event(ES_EVENT_TYPE_NOTIFY_CREATE, ES_ACTION_TYPE_NOTIFY, bxl->normalize_path_at(dirfd, linkPath, O_NOFOLLOW), bxl->GetProgramPath(), S_IFLNK);
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, event, report);
    return bxl->check_fwd_and_report_symlinkat(report, check, ERROR_RETURN_VALUE, target, dirfd, linkPath);
})

INTERPOSE_SOMETIMES(
    ssize_t,
    readlink,
    // rustc uses jemalloc
    // During it's initialization, jemalloc grabs a lock and then calls readlink on "/etc/malloc.conf"
    // libDomino hooks readlink
    // libDomino's readlink hook calls dlsym
    // dlsym calls calloc
    // calloc calls jemalloc
    // jemalloc tries to grab the lock, but this same thread already holds it.
    // To break this deadlock, we ideally would route this to real_readlink, but it is not initialized yet.
    // As a stopgap, we just assume it doesn't exist.
    if (path != NULL && (strcmp(path, "/etc/malloc.conf") == 0)) {
        errno = ENOENT;
        return -1;
    }, 
    const char *path, char *buf, size_t bufsize)(
{
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_READLINK, path, report, /*mode*/ 0, O_NOFOLLOW);
    return bxl->check_fwd_and_report_readlink(report, check, (ssize_t)ERROR_RETURN_VALUE, path, buf, bufsize);
})

INTERPOSE(ssize_t, readlinkat, int fd, const char *path, char *buf, size_t bufsize)({
    AccessReportGroup report;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_READLINK, fd, path, report, O_NOFOLLOW);
    return bxl->check_fwd_and_report_readlinkat(report, check, (ssize_t)ERROR_RETURN_VALUE, fd, path, buf, bufsize);
})

INTERPOSE(DIR*, opendir, const char *name)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, name, report);
    DIR *d = bxl->check_fwd_and_report_opendir(report, check, (DIR*)NULL, name);
    if (d) { bxl->reset_fd_table_entry(dirfd(d)); }
    return d;
})

INTERPOSE(DIR*, fdopendir, int fd)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd, report);
    return bxl->check_fwd_and_report_fdopendir(report, check, (DIR*)NULL, fd);
})

INTERPOSE(int, utime, const char *filename, const struct utimbuf *times)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_SETTIME, filename, report);
    return bxl->check_fwd_and_report_utime(report, check, ERROR_RETURN_VALUE, filename, times);
})

INTERPOSE(int, utimes, const char *filename, const struct timeval times[2])({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_SETTIME, filename, report);
    return bxl->check_fwd_and_report_utimes(report, check, ERROR_RETURN_VALUE, filename, times);
})

INTERPOSE(int, utimensat, int dirfd, const char *pathname, const struct timespec times[2], int flags)({
    AccessReportGroup report;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_SETTIME, dirfd, pathname, report);
    return bxl->check_fwd_and_report_utimensat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, times, flags);
})

INTERPOSE(int, futimens, int fd, const struct timespec times[2])({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_SETTIME, fd, report);
    return bxl->check_fwd_and_report_futimens(report, check, ERROR_RETURN_VALUE, fd, times);
})

INTERPOSE(int, futimesat, int dirfd, const char *pathname, const struct timeval times[2])({
    AccessReportGroup report;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_SETTIME, dirfd, pathname, report);
    return bxl->check_fwd_and_report_futimesat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, times);
})

static AccessCheckResult report_create(const char *syscall, BxlObserver *bxl, int dirfd, const char *pathname, mode_t mode, AccessReportGroup &report, bool checkCache = true)
{
    IOEvent event(ES_EVENT_TYPE_NOTIFY_CREATE, ES_ACTION_TYPE_NOTIFY, bxl->normalize_path_at(dirfd, pathname), bxl->GetProgramPath(), mode);
    return bxl->create_access(__func__, event, report, checkCache);
}

INTERPOSE(int, mkdir, const char *pathname, mode_t mode)({
    AccessReportGroup report;
    // We don't want to use the cache. Check comment in rmdir interposing for details.
    auto check = report_create(__func__, bxl, AT_FDCWD, pathname, S_IFDIR, report, /* checkCache */ false);
    return bxl->check_fwd_and_report_mkdir(report, check, ERROR_RETURN_VALUE, pathname, mode);
})

INTERPOSE(int, mkdirat, int dirfd, const char *pathname, mode_t mode)({
    AccessReportGroup report;
    // We don't want to use the cache. Check comment in rmdir interposing for details.
    auto check = report_create(__func__, bxl, dirfd, pathname, S_IFDIR, report, /* checkCache */ false);
    return bxl->check_fwd_and_report_mkdirat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, mode);
})

INTERPOSE(int, mknod, const char *pathname, mode_t mode, dev_t dev)({
    AccessReportGroup report;
    auto check = report_create(__func__, bxl, AT_FDCWD, pathname, S_IFREG, report);
    return bxl->check_fwd_and_report_mknod(report, check, ERROR_RETURN_VALUE, pathname, mode, dev);
})

INTERPOSE(int, mknodat, int dirfd, const char *pathname, mode_t mode, dev_t dev)({
    AccessReportGroup report;
    auto check = report_create(__func__, bxl, dirfd, pathname, S_IFREG, report);
    return bxl->check_fwd_and_report_mknodat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, mode, dev);
})

INTERPOSE(int, vprintf, const char *fmt, va_list args)({
    AccessReportGroup report;
    bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, 1, report);
    return bxl->fwd_vprintf(fmt, args).restore();
})

INTERPOSE(int, vfprintf, FILE *f, const char *fmt, va_list args)({
    AccessReportGroup report;
    bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fileno(f), report);
    return bxl->fwd_vfprintf(f, fmt, args).restore();
})

INTERPOSE(int, vdprintf, int fd, const char *fmt, va_list args)({
    AccessReportGroup report;
    bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd, report);
    return bxl->fwd_and_report_vdprintf(report, -1, fd, fmt, args).restore();
})

INTERPOSE(int, printf, const char *fmt, ...)({
    va_list args;
    va_start(args, fmt);
    result_t<int> result = vprintf(fmt, args);
    va_end(args);
    return result.restore();
})

INTERPOSE(int, fprintf, FILE *f, const char *fmt, ...)({
    va_list args;
    va_start(args, fmt);
    result_t<int> result = vfprintf(f, fmt, args);
    va_end(args);
    return result.restore();
})

INTERPOSE(int, dprintf, int fd, const char *fmt, ...)({
    va_list args;
    va_start(args, fmt);
    result_t<int> result = vdprintf(fd, fmt, args);
    va_end(args);
    return result.restore();
})

INTERPOSE(int, chmod, const char *pathname, mode_t mode)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_NOTIFY_SETMODE, pathname, report);
    return bxl->check_fwd_and_report_chmod(report, check, ERROR_RETURN_VALUE, pathname, mode);
})

INTERPOSE(int, fchmod, int fd, mode_t mode)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_SETMODE, fd, report);
    return bxl->check_fwd_and_report_fchmod(report, check, ERROR_RETURN_VALUE, fd, mode);
})

INTERPOSE(int, fchmodat, int dirfd, const char *pathname, mode_t mode, int flags)({
    AccessReportGroup report;
    int oflags = (flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_NOTIFY_SETMODE, dirfd, pathname, report, oflags);
    return bxl->check_fwd_and_report_fchmodat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, mode, flags);
})

INTERPOSE(void*, dlopen, const char *filename, int flags)({
    static int libcSoNameLength = -1;
    if (libcSoNameLength == -1) libcSoNameLength = strlen(LIBC_SO);

    if (filename && (strncmp(filename, LIBC_SO, libcSoNameLength) == 0))
    {
        BXL_LOG_DEBUG(bxl, "NOT forwarding dlopen(\"%s\", %d); returning dlopen(NULL, %d)", filename, flags, flags);
        return bxl->real_dlopen((char*)NULL, flags);
    }
    else
    {
        return bxl->fwd_dlopen(filename, flags).restore();
    }
})

INTERPOSE(int, chown, const char *pathname, uid_t owner, gid_t group)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_AUTH_SETOWNER, pathname, report);
    return bxl->check_fwd_and_report_chown(report, check, ERROR_RETURN_VALUE, pathname, owner, group);
})

INTERPOSE(int, fchown, int fd, uid_t owner, gid_t group)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_AUTH_SETOWNER, fd, report);
    return bxl->check_fwd_and_report_fchown(report, check, ERROR_RETURN_VALUE, fd, owner, group);
})

INTERPOSE(int, lchown, const char *pathname, uid_t owner, gid_t group)({
    AccessReportGroup report;
    auto check = bxl->create_access(__func__, ES_EVENT_TYPE_AUTH_SETOWNER, pathname, report, /*mode*/0, O_NOFOLLOW);
    return bxl->check_fwd_and_report_lchown(report, check, ERROR_RETURN_VALUE, pathname, owner, group);
})

INTERPOSE(int, chown32, const char *pathname, uid_t owner, gid_t group)({ return chown(pathname, owner, group); })
INTERPOSE(int, fchown32, int fd, uid_t owner, gid_t group)({ return fchown(fd, owner, group); })
INTERPOSE(int, lchown32, const char *pathname, uid_t owner, gid_t group)({ return lchown(pathname, owner, group); })

INTERPOSE(int, fchownat, int dirfd, const char *pathname, uid_t owner, gid_t group, int flags)({
    AccessReportGroup report;
    int oflags = (flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0;
    auto check = bxl->create_access_at(__func__, ES_EVENT_TYPE_AUTH_SETOWNER, dirfd, pathname, report, oflags);
    return bxl->check_fwd_and_report_fchownat(report, check, ERROR_RETURN_VALUE, dirfd, pathname, owner, group, flags);
})

INTERPOSE(ssize_t, sendfile, int out_fd, int in_fd, off_t *offset, size_t count)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, out_fd, report);
    return bxl->check_fwd_and_report_sendfile(report, check, (ssize_t)ERROR_RETURN_VALUE, out_fd, in_fd, offset, count);
})

INTERPOSE(ssize_t, sendfile64, int out_fd, int in_fd, off_t *offset, size_t count)({
    return sendfile(out_fd, in_fd, offset, count);
})

INTERPOSE(ssize_t, copy_file_range, int fd_in, loff_t *off_in, int fd_out, loff_t *off_out, size_t len, unsigned int flags)({
    AccessReportGroup report;
    auto check = bxl->create_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_WRITE, fd_out, report);
    ssize_t result;
    if (bxl->should_deny(check)) {
        errno = EPERM;
        result = (ssize_t)ERROR_RETURN_VALUE;
        goto exit;
    }

    // TODO: Remove the following workaround when the kernel bug is fixed.
    //
    // Due to (possibly) kernel bug, copy_file_range does no longer work when the file descriptors are not mounted on the same
    // filesystems, despite what is said in the manual https://man7.org/linux/man-pages/man2/copy_file_range.2.html.
    // This bug breaks AnyBuild virtual filesystem (VFS) because the source file will be in the read-only (lower) layer of overlayfs, and this
    // layer is mounted on AnyBuild FUSE, and the target file will be in the writable (upper) layer of overlayfs.
    //
    // In the commented code below, we try to check if the file descriptors are mounted on the same filesystem, and if so, we simply call
    // copy_file_range. On the user space, the descriptors are mounted on the same filesystem. However, when copy_file_range is called,
    // and the call goes into the kernel space, those descriptors are identified from different filesystems, and so the call will fail with EXDEV.
    //
    // ------------------------------------------------------------------------------------------
    // struct stat st_in;
    // if (fstat(fd_in, &st_in) == -1)
    //     return (ssize_t)ERROR_RETURN_VALUE;
    // struct stat st_out;
    // if (fstat(fd_out, &st_out) == -1)
    //     return (ssize_t)ERROR_RETURN_VALUE;
    // errno = 0;
    // if ((uintmax_t)major(st_in.st_dev) == (uintmax_t)major(st_out.st_dev)
    //     && (uintmax_t)minor(st_in.st_dev) == (uintmax_t)minor(st_out.st_dev))
    //     return bxl->fwd_copy_file_range(fd_in, off_in, fd_out, off_out, len, flags).restore();
    // ------------------------------------------------------------------------------------------

    // The code below implements copy_file_range using splice(2). The idea is the content is first
    // copied to the pipe and then transferred to the target.

    // Check for flags.
    if (flags != 0) {
        errno = EINVAL;
        result = (ssize_t)ERROR_RETURN_VALUE;
        goto exit;
    }

    // Check for overlapped range.
    if (fd_in == fd_out) {
        off64_t start_off_in = off_in == NULL ? lseek(fd_in, 0, SEEK_CUR) : *off_in;
        off64_t end_off_in = start_off_in + len;
        off64_t start_off_out = off_out == NULL ? lseek(fd_out, 0, SEEK_CUR) : *off_out;
        off64_t end_off_out = start_off_out + len;
        if ((start_off_in <= end_off_out && end_off_in >= start_off_out)
            || (start_off_out <= end_off_in && end_off_out >= start_off_in)) {
                errno = EINVAL;
                result = (ssize_t)ERROR_RETURN_VALUE;
                goto exit;
            }
    }

    errno = 0;

    // Creates a pipe.
    int pipefd[2];
    result = pipe(pipefd);
    if (result < 0)
        goto exit;

    // Copy from input to pipe.
    result = splice(fd_in, off_in, pipefd[1], NULL, len, 0);
    if (result < 0)
        goto exit;

    // Copy from pipe to output.
    result = splice(pipefd[0], NULL, fd_out, off_out, result, 0);

exit:
    close(pipefd[0]);
    close(pipefd[1]);
    report.SetErrno(result == -1? errno : 0);
    bxl->SendReport(report);
    
    return result;
})

INTERPOSE(int, name_to_handle_at, int dirfd, const char *pathname, struct file_handle *handle, int *mount_id, int flags)({
    int oflags = (flags & AT_SYMLINK_FOLLOW) ? 0 : O_NOFOLLOW;
    string pathStr = bxl->normalize_path_at(dirfd, pathname, oflags);
    AccessReportGroup report;
    auto check = CreateFileOpen(bxl, pathStr, oflags, report);
    return ret_fd(bxl->check_fwd_and_report_name_to_handle_at(report, check, ERROR_RETURN_VALUE, dirfd, pathname, handle, mount_id, flags), bxl);
})

INTERPOSE(int, close, int fd) ({ 
    bxl->reset_fd_table_entry(fd);
    return bxl->fwd_close(fd).restore();
})

INTERPOSE(int, fclose, FILE *f) ({
    bxl->reset_fd_table_entry(fileno(f));
    return bxl->fwd_fclose(f).restore();
})

INTERPOSE(int, closedir, DIR *dirp) ({ 
    bxl->reset_fd_table_entry(dirfd(dirp));
    return bxl->fwd_closedir(dirp).restore();
})

INTERPOSE(int, dup, int fd) ({ 
    return ret_fd(bxl->real_dup(fd), bxl);    
    // Sometimes useful (for debugging) to interpose without access checking:
    // return bxl->fwd_dup(fd).restore();     
})

INTERPOSE(int, dup2, int oldfd, int newfd)({
    // If the file descriptor newfd was previously open, it is closed
    // before being reused; the close is performed silently, so we should reset the fd table.
    bxl->reset_fd_table_entry(newfd);

    return bxl->real_dup2(oldfd, newfd); 
    // Sometimes useful (for debugging) to interpose without access checking:
    // return bxl->fwd_dup2(oldfd, newfd).restore();  
})

INTERPOSE(int, dup3, int oldfd, int newfd, int flags)({
    // If the file descriptor newfd was previously open, it is closed
    // before being reused; the close is performed silently, so we should reset the fd table.
    bxl->reset_fd_table_entry(newfd);

    return bxl->real_dup3(oldfd, newfd, flags); 
    // Sometimes useful (for debugging) to interpose without access checking:
    //return bxl->fwd_dup3(oldfd, newfd).restore();  
})

static void report_exit(int exitCode, void *args)
{
    BxlObserver::GetInstance()->SendExitReport();
}

// invoked by the loader when our shared library is dynamically loaded into a new host process
void __attribute__ ((constructor)) _bxl_linux_sandbox_init(void)
{
    // set up an on-exit handler
    on_exit(report_exit, NULL);

    // report that a new process has been created 
    BxlObserver::GetInstance()->report_access("__init__", ES_EVENT_TYPE_NOTIFY_EXEC, __progname);
}

// ==========================

// having a main function is useful for various local testing
int main(int argc, char **argv)
{
    BxlObserver *inst = BxlObserver::GetInstance();
    printf("Path: %s\n", inst->GetReportsPath());
}

/* ============ don't need to be interposed =======================

INTERPOSE(int, statfs, const char *pathname, struct statfs *buf)({
    result_t<int> result = bxl->fwd_statfs(pathname, buf);
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname);
    return result.restore();
})

INTERPOSE(int, statfs64, const char *pathname, struct statfs64 *buf)({
    result_t<int> result = bxl->fwd_statfs64(pathname, buf);
    bxl->report_access(__func__, ES_EVENT_TYPE_NOTIFY_STAT, pathname);
    return result.restore();
})

INTERPOSE(int, fstatfs, int fd, struct statfs *buf)({
    result_t<int> result = bxl->fwd_fstatfs(fd, buf);
    bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd);
    return result.restore();
})

INTERPOSE(int, fstatfs64, int fd, struct statfs64 *buf)({
    result_t<int> result = bxl->fwd_fstatfs64(fd, buf);
    bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_STAT, fd);
    return result.restore();
})

=================================================================== */

/* ============ old/obsolete/unavailable ==========================

INTERPOSE(int, execveat, int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags)({
    int oflags = (flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0;
    string exe_path = bxl->normalize_path_at(dirfd, pathname, oflags);
    bxl->report_exec(__func__, argv[0], exe_path.c_str());
    return bxl->fwd_execveat(dirfd, pathname, argv, bxl->ensureEnvs(envp), flags).restore();
})

INTERPOSE(int, getdents, unsigned int fd, struct linux_dirent *dirp, unsigned int count)({
    auto check = bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, fd);
    return bxl->check_and_fwd_getdents(check, ERROR_RETURN_VALUE, fd, dirp, count);
})

INTERPOSE(int, getdents64, unsigned int fd, struct linux_dirent64 *dirp, unsigned int count)({
    auto check = bxl->report_access_fd(__func__, ES_EVENT_TYPE_NOTIFY_READDIR, fd);
    return bxl->check_and_fwd_getdents64(check, ERROR_RETURN_VALUE, fd, dirp, count);
})

=================================================================== */
