// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "dirent.h"
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stddef.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/vfs.h>
#include <utime.h>

#include <ostream>
#include <sstream>
#include <chrono>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "Sandbox.hpp"
#include "SandboxedPip.hpp"
#include "utils.h"
#include "common.h"

/*
 * This header is compiled into two different libraries: libDetours.so and libAudit.so.
 *
 * When compiling libDetours.so, the ENABLE_INTERPOSING macro is defined, otherwise it is not.
 *
 * When ENABLE_INTERPOSING is defined, we do not need static declarations for the system calls of interest, because
 * we resolve those dynamically via `dlsym(name)` calls.  That means that, even though we compile libDetours.so against
 * glibc 2.17 (where, for example, `copy_file_range` is not defined), when our libDetours.so is loaded into a process that
 * runs against a newer version of glibc, `dlsym("copy_file_range")` will still return a valid function pointer and we 
 * will be able to interpose system calls that are not necessarily present in the glibc 2.17.
 * 
 * When ENABLE_INTERPOSING is not defined, we need static definitions for all the system calls we reference.  Therefore, 
 * here we need to add fake definitions for the calls that we want to reference (because of libDetours) which are not present
 * in glibc we are compiling against.  Adding empty definitions here is fine as long as in our code we never explicitly call
 * the corresponding real_<missing-syscall> instance methods in the BxlObserver class.
 */
#ifndef ENABLE_INTERPOSING
    // Library support for copy_file_range was added in glibc 2.27 (https://man7.org/linux/man-pages/man2/copy_file_range.2.html)
    #if __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 27)
    inline ssize_t copy_file_range(int fd_in, loff_t *off_in, int fd_out, loff_t *off_out, size_t len, unsigned int flags) {
        return -1;
    }
    #endif

    // Library support for pwritev2 was added in glibc 2.26 (https://man7.org/linux/man-pages/man2/pwritev2.2.html)
    #if __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 26)
    inline ssize_t pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
        return -1;
    }
    #endif
#endif

using namespace std;

extern const char *__progname;

static const char LD_PRELOAD_ENV_VAR_PREFIX[] = "LD_PRELOAD=";

#define ARRAYSIZE(arr) (sizeof(arr)/sizeof(arr[0]))

#ifdef ENABLE_INTERPOSING
    #define GEN_FN_DEF_REAL(ret, name, ...)                                         \
        typedef ret (*fn_real_##name)(__VA_ARGS__);                                 \
        const fn_real_##name real_##name = (fn_real_##name)dlsym(RTLD_NEXT, #name);

    #define MAKE_BODY(B) \
        B \
    }

    // It's important to have an option to bail out early, *before*
    // the call to BxlObserver::GetInstance() because we might not
    // have the process initialized far enough for that call to succeed.
    #define INTERPOSE_SOMETIMES(ret, name, short_circuit_check, ...) \
        DLL_EXPORT ret name(__VA_ARGS__) {                           \
            short_circuit_check                                      \
            BxlObserver *bxl = BxlObserver::GetInstance();           \
            BXL_LOG_DEBUG(bxl, "Intercepted %s", #name);             \
            MAKE_BODY

    #define INTERPOSE(ret, name, ...) \
        INTERPOSE_SOMETIMES(ret, name, ;, __VA_ARGS__)
#else
    #define GEN_FN_DEF_REAL(ret, name, ...)         \
        typedef ret (*fn_real_##name)(__VA_ARGS__); \
        const fn_real_##name real_##name = (fn_real_##name)name;

    #define IGNORE_BODY(B)

    #define INTERPOSE(ret, name, ...) IGNORE_BODY
#endif

// Linux libraries are required to set errno only when the operation fails. 
// In most cases, when the operation succeeds errno is set to a random value
// (or it does not get updated at all). Therefore, only report errno when   
// the operation fails, and otherwise return 0. This allows the managed     
// side of BuildXL to interpret when an operation succeeds or fails, and    
// to retrieve the details in case of the failure.                                                                                         
#define GEN_FN_DEF(ret, name, ...)                                              \
    GEN_FN_DEF_REAL(ret, name, __VA_ARGS__)                                     \
    template<typename ...TArgs> result_t<ret> fwd_##name(TArgs&& ...args)       \
    {                                                                           \
        ret result = real_##name(std::forward<TArgs>(args)...);                 \
        result_t<ret> return_value(result);                                     \
        LOG_DEBUG("Forwarded syscall %s (errno: %d)",                           \
            RenderSyscall(#name, result, std::forward<TArgs>(args)...).c_str(), \
            return_value.get_errno());                                          \
        return return_value;                                                    \
    }                                                                           \  
    template<typename ...TArgs> ret check_fwd_and_report_##name(                \
        AccessReportGroup& report,                                              \
        AccessCheckResult &check,                                               \
        ret error_val,                                                          \
        TArgs&& ...args)                                                        \
    {                                                                           \
        result_t<ret> return_value = should_deny(check)                         \
            ? result_t<ret>(error_val, EPERM)                                   \
            : fwd_##name(args...);                                              \
        report.SetErrno(return_value.get() == error_val                         \
            ? return_value.get_errno()                                          \
            : 0);                                                               \
        BxlObserver::GetInstance()->SendReport(report);                         \
        return return_value.restore();                                          \
    }                                                                           \
    template<typename ...TArgs> result_t<ret> fwd_and_report_##name(            \
        AccessReportGroup& report,                                              \
        ret error_val,                                                          \
        TArgs&& ...args)                                                        \
    {                                                                           \
        result_t<ret> return_value = fwd_##name(args...);                       \
        report.SetErrno(return_value.get() == error_val                         \
            ? return_value.get_errno()                                          \
            : 0);                                                               \
        BxlObserver::GetInstance()->SendReport(report);                         \
        return return_value;                                                    \
    }                                                                           \

#define _fatal(fmt, ...) do { real_fprintf(stderr, "(%s) " fmt "\n", __func__, __VA_ARGS__); _exit(1); } while (0)
#define fatal(msg) _fatal("%s", msg)

#define _fatal_undefined_env(name)                                                                      \
    char** procenv = environ;                                                                           \
    std::stringstream ss;                                                                               \
    for (int i = 0; procenv[i] != NULL; i++) {                                                          \
        ss << procenv[i];                                                                               \
        if (procenv[i+1] != NULL) {                                                                     \
            ss << ",";                                                                                  \
        }                                                                                               \
    }                                                                                                   \
    _fatal("[%s] ERROR: Env var '%s' not set. Environment: [%s]\n", __func__, name, ss.str().c_str());  \

/**
 * Wraps the result of a syscall together with the current 'errno'.
 *
 * When 'restore' is called, if allowed, 'errno' is reset back to
 * the value that was captured in the constructor and the captured result is returned;
 * otherwise 'errno' is set to EPERM and the error value is returned.
 */
template <typename T>
class result_t final
{
private:
    int my_errno_;
    T result_;

public:
    result_t(T result) : result_(result), my_errno_(errno) {}
    result_t(T result, int error) : result_(result), my_errno_(error) {}

    /** Returns the remembered result and restores 'errno' to the value captured in the constructor. */
    inline T restore()
    {
        errno = my_errno_;
        return result_;
    }

    /** Returns the remembered result. */
    inline T get()
    {
        return result_;
    }

    /** Returns the remembered errno. */
    inline int get_errno()
    {
        return my_errno_;
    }
};

/**
 * Singleton class responsible for reporting accesses.
 *
 * Accesses are observed by intercepting syscalls.
 *
 * Accesses are reported to a file (can be a regular file or a FIFO)
 * at the location specified by the FileAccessManifest.
 */
class BxlObserver final
{
private:
    BxlObserver();
    ~BxlObserver() { disposed_ = true; }
    BxlObserver(const BxlObserver&) = delete;
    BxlObserver& operator = (const BxlObserver&) = delete;

    volatile int disposed_;
    int rootPid_;
    char progFullPath_[PATH_MAX];
    char detoursLibFullPath_[PATH_MAX];
    char famPath_[PATH_MAX];
    char ptraceMqName_[NAME_MAX];
    char forcedPTraceProcessNamesList_[PATH_MAX];

    std::timed_mutex cacheMtx_;
    std::unordered_map<es_event_type_t, std::unordered_set<std::string>> cache_;

    // In a typical case, a process will not have more than 1024 open file descriptors at a time.
    // File descriptors start at 3 (1 and 2 are reserved for stdout and stderr).
    // Whenever a new file descriptor is created, the smallest available positive integer is assigned to it. 
    // Whenever a file descriptor is closed, its value is returned to the pool and will be used for new ones.
    // Setting the size of this table to 1024 should accommodate most of the common cases.
    // File descriptors can be greater than 1024, and if that happens we just won't cache their paths.
    static const int MAX_FD = 1024;
    std::string fdTable_[MAX_FD];
    const char* const empty_str_ = "";
    bool useFdTable_ = true;

    std::shared_ptr<SandboxedPip> pip_;
    std::shared_ptr<SandboxedProcess> process_;
    Sandbox *sandbox_;

    // Cache for statically linked processes in the form <timestamp>:<path>
    std::vector<std::pair<std::string, bool>> staticallyLinkedProcessCache_;
    std::vector<std::string> forcedPTraceProcessNames_;

    void InitFam();
    void InitDetoursLibPath();
    void InitPTraceMq();
    bool Send(const char *buf, size_t bufsiz);
    bool IsCacheHit(es_event_type_t event, const string &path, const string &secondPath);
    char** ensure_env_value_with_log(char *const envp[], char const *envName, const char *envValue);
    void report_access_internal(const char *syscallName, es_event_type_t eventType, const char *reportPath, const char *secondPath = nullptr, mode_t mode = 0, int error = 0, bool checkCache = true);
    AccessCheckResult create_access_internal(const char *syscallName, es_event_type_t eventType, const char *reportPath, const char *secondPath, AccessReportGroup &reportGroup, mode_t mode = 0, bool checkCache = true);
    ssize_t read_path_for_fd(int fd, char *buf, size_t bufsiz, pid_t associatedPid = 0);

    bool IsMonitoringChildProcesses() const { return !pip_ || CheckMonitorChildProcesses(pip_->GetFamFlags()); }
    bool IsPTraceEnabled() const { return pip_ && (CheckEnableLinuxPTraceSandbox(pip_->GetFamExtraFlags()) || CheckUnconditionallyEnableLinuxPTraceSandbox(pip_->GetFamExtraFlags())); }
    bool IsPTraceForced(const char *path);

    inline bool IsValid() const             { return sandbox_ != NULL; }
    inline bool IsEnabled() const
    {
        return
            // successfully initialized
            IsValid() &&
            // NOT (child processes should break away AND this is a child process)
            !(pip_->AllowChildProcessesToBreakAway() && getpid() != rootPid_);
    }

    void PrintArgs(std::stringstream& str, bool isFirst)
    {
    }

    template<typename TFirst, typename ...TRest>
    void PrintArgs(std::stringstream& str, bool isFirst, TFirst first, const TRest& ...rest)
    {
        if (!isFirst) str << ", ";
        str << first;
        PrintArgs(str, false, rest...);
    }

    template<typename TRet, typename ...TArgs>
    std::string RenderSyscall(const char *syscallName, const TRet& retVal, const TArgs& ...args)
    {
        std::stringstream str;
        str << syscallName << "(";
        PrintArgs(str, true, args...);
        str << ") = " << retVal;
        return str.str();
    }

    void resolve_path(char *fullpath, bool followFinalSymlink);

    // Builds the report to be sent over the FIFO in the given buffer
    inline int BuildReport(char* buffer, int maxMessageLength, const AccessReport &report, const char *path)
    {
        return snprintf(
            buffer, maxMessageLength, "%s|%d|%d|%d|%d|%d|%d|%s|%d\n",
            __progname, report.pid < 0 ? getpid() : report.pid, report.requestedAccess, report.status, report.reportExplicitly, report.error, report.operation, path, report.isDirectory);
    }

    static BxlObserver *sInstance;
    static AccessCheckResult sNotChecked;

#if _DEBUG
   #define BXL_LOG_DEBUG(bxl, fmt, ...) if (bxl->LogDebugEnabled()) bxl->LogDebug("[%s:%d] " fmt, __progname, getpid(), __VA_ARGS__);
#else
    #define BXL_LOG_DEBUG(bxl, fmt, ...)
#endif

#define LOG_DEBUG(fmt, ...) BXL_LOG_DEBUG(this, fmt, __VA_ARGS__)

public:
    static BxlObserver* GetInstance();

    bool SendReport(const AccessReport &report, bool isDebugMessage = false);
    bool SendReport(const AccessReportGroup &report);
    // Specialization for the exit report event. 
    // We may need to send an exit report on exit handlers after destructors
    // have been called. This method avoids accessing shared structures.
    bool SendExitReport(pid_t pid = 0);
    char** ensureEnvs(char *const envp[]);

    const char* GetProgramPath() { return progFullPath_; }
    const char* GetReportsPath() { int len; return IsValid() ? pip_->GetReportsPath(&len) : NULL; }
    const char* GetDetoursLibPath() { return detoursLibFullPath_; }

    void report_exec(const char *syscallName, const char *procName, const char *file, int error, mode_t mode = 0);
    void report_audit_objopen(const char *fullpath)
    {
        IOEvent event(ES_EVENT_TYPE_NOTIFY_OPEN, ES_ACTION_TYPE_NOTIFY, fullpath, progFullPath_, S_IFREG);
        report_access("la_objopen", event, /* checkCache */ true);
    }

    // Removes detours path from LD_PRELOAD from the given environment and returns the modified environment
    inline char** RemoveLDPreloadFromEnv(char *const envp[])
    { 
        return remove_path_from_LDPRELOAD(envp, detoursLibFullPath_);
    }

    // TODO [pgunasekara]: All of the create_access/report_access functions below should take associatedPid as an argument
    //                     When running with ptrace the tracer that does the reports has a different pid from the tracee, which is the process being traced.
    //                     Thus, to have a correct report, e.g., to get the tracee's working dir, the tracer needs to know the pid of the tracee, which is passed as the associatedpid parameter
    // The following functions create an access report and performs an access check. They do not report the created access to managed BuildXL.
    // The created access report is returned as an out param in the given 'report' param. The returned report is ready to be sent with the exception of
    // setting the operation error. In operations where the error is reported back, the typical flow is creating the report, performing the operation, 
    // setting errno in the report and sending out the report.
    AccessCheckResult create_access(const char *syscallName, IOEvent &event, AccessReportGroup &report, bool checkCache = true);
    // In this method (and immediately below) 'mode' is provided on a best effort basis. If 0 is passed for mode, it will be
    // explicitly computed
    AccessCheckResult create_access(const char *syscallName, es_event_type_t eventType, const char *pathname, AccessReportGroup &report, mode_t mode = 0, int oflags = 0, bool checkCache = true, pid_t associatedPid = 0);
    AccessCheckResult create_access(const char *syscallName, es_event_type_t eventType, const char *reportPath, const char *secondPath, AccessReportGroup &reportGroup, mode_t mode = 0, bool checkCache = true, pid_t associatedPid = 0);
    AccessCheckResult create_access_fd(const char *syscallName, es_event_type_t eventType, int fd, AccessReportGroup &reportGroup);
    AccessCheckResult create_access_at(const char *syscallName, es_event_type_t eventType, int dirfd, const char *pathname, AccessReportGroup &reportGroup, int oflags = 0, bool getModeWithFd = true, pid_t associatedPid = 0);

    // The following functions are the create_* equivalent of the ones above but the access is reported to managed BuildXL
    void report_access(const char *syscallName, IOEvent &event, bool checkCache = true);
    void report_access(const char *syscallName, es_event_type_t eventType, const char *pathname, mode_t mode = 0, int oflags = 0, int error = 0, bool checkCache = true, pid_t associatedPid = 0);
    void report_access(const char *syscallName, es_event_type_t eventType, const char *reportPath, const char *secondPath, mode_t mode = 0, int error = 0, bool checkCache = true);
    void report_access_fd(const char *syscallName, es_event_type_t eventType, int fd, int error);
    void report_access_at(const char *syscallName, es_event_type_t eventType, int dirfd, const char *pathname, int oflags, bool getModeWithFd = true, pid_t associatedPid = 0, int error = 0);

    // Send a special message to managed code if the policy to override allowed writes based on file existence is set
    // and the write is allowed by policy
    void report_firstAllowWriteCheck(const char *fullPath);

    // Checks and reports when a statically linked binary is about to be executed
    bool check_and_report_statically_linked_process(const char *path);
    bool check_and_report_statically_linked_process(int fd);
    bool is_statically_linked(const char *path);

    // Clears the specified entry on the file descriptor table
    void reset_fd_table_entry(int fd);
    
    // Clears the entire file descriptor table
    void reset_fd_table();

    // Disables the FD table. Cannot be re-enabled for the remainder of the sandbox lifetime.
    void disable_fd_table();
    
    // Returns the path associated with the given file descriptor
    // Note: This function assumes fd is a file descriptor pointing to a regular file (that is, a file, directory or symlink, not a pipe/socket/etc). The reason for this assumption is that file descriptors
    // are cached and the corresponding invalidation is tied to opening handles against file names. We are currently not detouring pipe creation, so we run the risk of not invalidating the file descriptor
    // table properly for the case of pipes when we miss a close.
    std::string fd_to_path(int fd, pid_t associatedPid = 0);
    
    std::string normalize_path_at(int dirfd, const char *pathname, int oflags = 0, pid_t associatedPid = 0);

    // Whether the given descriptor is a non-file (e.g., a pipe, or socket, etc.)
    static bool is_non_file(const mode_t mode);

    // Enumerates a specified directory
    bool EnumerateDirectory(std::string rootDirectory, bool recursive, std::vector<std::string>& filesAndDirectories);

    const char* getPTraceMqName() const { return IsPTraceEnabled() ? ptraceMqName_ : ""; }
    const char* getFamPath() const { return famPath_; };

    inline bool LogDebugEnabled()
    {
        if (pip_ == NULL)
        {
            // The observer isn't initialized yet. We're being defensive here,
            // in case someone adds a LOG_DEBUG in a place where it would cause a segfault. 
            return false;
        }

        return CheckEnableLinuxSandboxLogging(pip_->GetFamExtraFlags());
    }

    void LogDebug(const char *fmt, ...);

    mode_t get_mode(const char *path)
    {
        int old = errno;
        struct stat buf;
#if (__GLIBC__ == 2 && __GLIBC_MINOR__ < 33)
        mode_t result = real___lxstat(1, path, &buf) == 0
#else
        mode_t result = real_lstat(path, &buf) == 0
#endif
            ? buf.st_mode
            : 0;
        errno = old;
        return result;
    }

    mode_t get_mode(int fd)
    {
        int old = errno;
        struct stat buf;
#if (__GLIBC__ == 2 && __GLIBC_MINOR__ < 33)
        mode_t result = real___fxstat(1, fd, &buf) == 0
#else
        mode_t result = real_fstat(fd, &buf) == 0
#endif
            ? buf.st_mode
            : 0;
        errno = old;
        return result;
    }

    char *getcurrentworkingdirectory(char *fullpath, size_t size, pid_t associatedPid = 0)
    {
        if (associatedPid == 0)
        {
            return getcwd(fullpath, size);
        }
        else
        {
            char linkPath[100] = {0};
            sprintf(linkPath, "/proc/%d/cwd", associatedPid);
            if (real_readlink(linkPath, fullpath, size) == -1)
            {
                return NULL;
            }
            
            return fullpath;
        }
    }

    std::string normalize_path(const char *pathname, int oflags = 0, pid_t associatedPid = 0)
    {
        if (pathname == nullptr)
        {
            return empty_str_;
        }

        return normalize_path_at(AT_FDCWD, pathname, oflags, associatedPid);
    }

    bool IsFailingUnexpectedAccesses()
    {
        return CheckFailUnexpectedFileAccesses(pip_->GetFamFlags());
    }

    /**
     * Returns whether the given access should be denied.
     *
     * This is true when
     *   - the given access is not permitted
     *   - the sandbox is configured to deny accesses that are not permitted
     */
    bool should_deny(AccessCheckResult &check)
    {
        return IsEnabled() && check.ShouldDenyAccess() && IsFailingUnexpectedAccesses();
    }

    GEN_FN_DEF(void*, dlopen, const char *filename, int flags);
    GEN_FN_DEF(int, dlclose, void *handle);

    GEN_FN_DEF(pid_t, fork, void);
    GEN_FN_DEF(int, clone, int (*fn)(void *), void *child_stack, int flags, void *arg, ... /* pid_t *ptid, void *newtls, pid_t *ctid */ );
    GEN_FN_DEF_REAL(void, _exit, int);
    GEN_FN_DEF(int, fexecve, int, char *const[], char *const[]);
    GEN_FN_DEF(int, execv, const char *, char *const[]);
    GEN_FN_DEF(int, execve, const char *, char *const[], char *const[]);
    GEN_FN_DEF(int, execvp, const char *, char *const[]);
    GEN_FN_DEF(int, execvpe, const char *, char *const[], char *const[]);
    GEN_FN_DEF(int, execl, const char *, const char *, ...);
    GEN_FN_DEF(int, execlp, const char *, const char *, ...);
    GEN_FN_DEF(int, execle, const char *, const char *, ...);
#if (__GLIBC__ == 2 && __GLIBC_MINOR__ < 33)
    GEN_FN_DEF(int, __lxstat, int, const char *, struct stat *);
    GEN_FN_DEF(int, __lxstat64, int, const char*, struct stat64*);
    GEN_FN_DEF(int, __xstat, int, const char *, struct stat *);
    GEN_FN_DEF(int, __xstat64, int, const char*, struct stat64*);
    GEN_FN_DEF(int, __fxstat, int, int, struct stat*);
    GEN_FN_DEF(int, __fxstatat, int, int, const char*, struct stat*, int);;
    GEN_FN_DEF(int, __fxstat64, int, int, struct stat64*);
    GEN_FN_DEF(int, __fxstatat64, int, int, const char*, struct stat64*, int);
#else
    GEN_FN_DEF(int, stat, const char *, struct stat *);
    GEN_FN_DEF(int, stat64, const char *, struct stat64 *);
    GEN_FN_DEF(int, lstat, const char *, struct stat *);
    GEN_FN_DEF(int, lstat64, const char *, struct stat64 *);
    GEN_FN_DEF(int, fstat, int, struct stat *);
    GEN_FN_DEF(int, fstat64, int, struct stat64 *);
#endif
    GEN_FN_DEF(FILE*, fdopen, int, const char *);
    GEN_FN_DEF(FILE*, fopen, const char *, const char *);
    GEN_FN_DEF(FILE*, fopen64, const char *, const char *);
    GEN_FN_DEF(FILE*, freopen, const char *, const char *, FILE *);
    GEN_FN_DEF(FILE*, freopen64, const char *, const char *, FILE *);
    GEN_FN_DEF(size_t, fread, void*, size_t, size_t, FILE*);
    GEN_FN_DEF(size_t, fwrite, const void*, size_t, size_t, FILE*);
    GEN_FN_DEF(int, fputc, int c, FILE *stream);
    GEN_FN_DEF(int, fputs, const char *s, FILE *stream);
    GEN_FN_DEF(int, putc, int c, FILE *stream);
    GEN_FN_DEF(int, putchar, int c);
    GEN_FN_DEF(int, puts, const char *s);
    GEN_FN_DEF(int, access, const char *, int);
    GEN_FN_DEF(int, faccessat, int, const char *, int, int);
    GEN_FN_DEF(int, creat, const char *, mode_t);
    GEN_FN_DEF(int, open64, const char *, int, mode_t);
    GEN_FN_DEF(int, open, const char *, int, mode_t);
    GEN_FN_DEF(int, openat, int, const char *, int, mode_t);
    GEN_FN_DEF(ssize_t, write, int, const void*, size_t);
    GEN_FN_DEF(ssize_t, writev, int fd, const struct iovec *iov, int iovcnt);
    GEN_FN_DEF(ssize_t, pwritev, int fd, const struct iovec *iov, int iovcnt, off_t offset);
    GEN_FN_DEF(ssize_t, pwritev2, int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);
    GEN_FN_DEF(ssize_t, pwrite, int fd, const void *buf, size_t count, off_t offset);
    GEN_FN_DEF(ssize_t, pwrite64, int fd, const void *buf, size_t count, off_t offset);
    GEN_FN_DEF(int, remove, const char *);
    GEN_FN_DEF(int, truncate, const char *path, off_t length);
    GEN_FN_DEF(int, ftruncate, int fd, off_t length);
    GEN_FN_DEF(int, truncate64, const char *path, off_t length);
    GEN_FN_DEF(int, ftruncate64, int fd, off_t length);
    GEN_FN_DEF(int, rmdir, const char *pathname);
    GEN_FN_DEF(int, rename, const char *, const char *);
    GEN_FN_DEF(int, renameat, int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
    GEN_FN_DEF(int, link, const char *, const char *);
    GEN_FN_DEF(int, linkat, int, const char *, int, const char *, int);
    GEN_FN_DEF(int, unlink, const char *pathname);
    GEN_FN_DEF(int, unlinkat, int dirfd, const char *pathname, int flags);
    GEN_FN_DEF(int, symlink, const char *, const char *);
    GEN_FN_DEF(int, symlinkat, const char *, int, const char *);
    GEN_FN_DEF(ssize_t, readlink, const char *, char *, size_t);
    GEN_FN_DEF(ssize_t, readlinkat, int, const char *, char *, size_t);
    GEN_FN_DEF(char*, realpath, const char*, char*);
    GEN_FN_DEF(DIR*, opendir, const char*);
    GEN_FN_DEF(DIR*, fdopendir, int);
    GEN_FN_DEF(int, utime, const char *filename, const struct utimbuf *times);
    GEN_FN_DEF(int, utimes, const char *filename, const struct timeval times[2]);
    GEN_FN_DEF(int, utimensat, int, const char*, const struct timespec[2], int);
    GEN_FN_DEF(int, futimesat, int dirfd, const char *pathname, const struct timeval times[2]);
    GEN_FN_DEF(int, futimens, int, const struct timespec[2]);
    GEN_FN_DEF(int, mkdir, const char*, mode_t);
    GEN_FN_DEF(int, mkdirat, int, const char*, mode_t);
    GEN_FN_DEF(int, mknod, const char *pathname, mode_t mode, dev_t dev);
    GEN_FN_DEF(int, mknodat, int dirfd, const char *pathname, mode_t mode, dev_t dev);
    GEN_FN_DEF(int, printf, const char*, ...);
    GEN_FN_DEF(int, fprintf, FILE*, const char*, ...);
    GEN_FN_DEF(int, dprintf, int, const char*, ...);
    GEN_FN_DEF(int, vprintf, const char*, va_list);
    GEN_FN_DEF(int, vfprintf, FILE*, const char*, va_list);
    GEN_FN_DEF(int, vdprintf, int, const char*, va_list);
    GEN_FN_DEF(int, chmod, const char *pathname, mode_t mode);
    GEN_FN_DEF(int, fchmod, int fd, mode_t mode);
    GEN_FN_DEF(int, fchmodat, int dirfd, const char *pathname, mode_t mode, int flags);
    GEN_FN_DEF(int, chown, const char *pathname, uid_t owner, gid_t group);
    GEN_FN_DEF(int, fchown, int fd, uid_t owner, gid_t group);
    GEN_FN_DEF(int, lchown, const char *pathname, uid_t owner, gid_t group);
    GEN_FN_DEF(int, fchownat, int dirfd, const char *pathname, uid_t owner, gid_t group, int flags);
    GEN_FN_DEF(ssize_t, sendfile, int out_fd, int in_fd, off_t *offset, size_t count);
    GEN_FN_DEF(ssize_t, sendfile64, int out_fd, int in_fd, off_t *offset, size_t count);
    GEN_FN_DEF(ssize_t, copy_file_range, int fd_in, loff_t *off_in, int fd_out, loff_t *off_out, size_t len, unsigned int flags);
    GEN_FN_DEF(int, name_to_handle_at, int dirfd, const char *pathname, struct file_handle *handle, int *mount_id, int flags);
    GEN_FN_DEF(int, dup, int oldfd);
    GEN_FN_DEF(int, dup2, int oldfd, int newfd);
    GEN_FN_DEF(int, dup3, int oldfd, int newfd, int flags);
    GEN_FN_DEF(int, scandir, const char * dirp, struct dirent *** namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **));
    GEN_FN_DEF(int, scandir64, const char * dirp, struct dirent64 *** namelist, int (*filter)(const struct dirent64  *), int (*compar)(const dirent64 **, const dirent64 **));
    GEN_FN_DEF(int, scandirat, int dirfd, const char * dirp, struct dirent *** namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **));
    GEN_FN_DEF(int, scandirat64, int dirfd, const char * dirp, struct dirent64 *** namelist, int (*filter)(const struct dirent64  *), int (*compar)(const dirent64 **, const dirent64 **));
    GEN_FN_DEF(int, statx, int dirfd, const char * pathname, int flags, unsigned int mask, struct statx * statxbuf);
    GEN_FN_DEF(int, closedir, DIR *dirp);
    GEN_FN_DEF(struct dirent *, readdir, DIR *dirp);
    GEN_FN_DEF(struct dirent64 *, readdir64, DIR *dirp);
    GEN_FN_DEF(int, readdir_r, DIR *dirp, struct dirent *entry, struct dirent **result);
    GEN_FN_DEF(int, readdir64_r, DIR *dirp, struct dirent64 *entry, struct dirent64 **result);

    /* ============ don't need to be interposed ======================= */
    GEN_FN_DEF(int, close, int fd);
    GEN_FN_DEF(int, fclose, FILE *stream);
    GEN_FN_DEF(int, statfs, const char *, struct statfs *buf);
    GEN_FN_DEF(int, statfs64, const char *, struct statfs64 *buf);
    GEN_FN_DEF(int, fstatfs, int fd, struct statfs *buf);
    GEN_FN_DEF(int, fstatfs64, int fd, struct statfs64 *buf);
    GEN_FN_DEF(FILE*, popen, const char *command, const char *type);
    GEN_FN_DEF(int, pclose, FILE *stream);
    /* =================================================================== */

    /* ============ old/obsolete/unavailable ==========================
    GEN_FN_DEF(int, execveat, int dirfd, const char *pathname, char *const argv[], char *const envp[], int flags);
    GEN_FN_DEF(int, renameat2, int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags);
    GEN_FN_DEF(int, getdents, unsigned int fd, struct linux_dirent *dirp, unsigned int count);
    GEN_FN_DEF(int, getdents64, unsigned int fd, struct linux_dirent64 *dirp, unsigned int count);
    =================================================================== */
};
