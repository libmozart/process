/**
 * Mozart++ Template Library
 * Licensed under MIT License
 * Copyright (c) 2020 Covariant Institute
 * Website: https://covariant.cn/
 * Github:  https://github.com/covariant-institute/
 */
#include <mozart++/core>

#ifndef MOZART_PLATFORM_WIN32

#include <mozart++/process>
#include <mozart++/string>
#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <cctype>
#include <climits>
#include <sys/stat.h>
#include <sys/wait.h>
#include <csignal>

#ifdef __APPLE__
#define FD_DIR "/dev/fd"
#define dirent64 dirent
#define readdir64 readdir
#else
#define FD_DIR "/proc/self/fd"
#endif

#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

namespace mpp_impl {
    static constexpr int FAIL_FILENO = STDERR_FILENO + 1;

    static bool close_all_descriptors() {
        DIR *dp = nullptr;
        struct dirent64 *dirp = nullptr;
        int from_fd = FAIL_FILENO + 1;

        // We're trying to close all file descriptors, but opendir() might\
        // itself be implemented using a file descriptor, and we certainly
        // don't want to close that while it's in use.  We assume that if
        // opendir() is implemented using a file descriptor, then it uses
        // the lowest numbered file descriptor, just like open().  So we
        // close a couple explicitly.

        // for possible use by opendir()
        close(from_fd);
        // another one for good luck
        close(from_fd + 1);

        if ((dp = opendir(FD_DIR)) == nullptr) {
            return false;
        }

        // use readdir64 in case of fd > 1024
        while ((dirp = readdir64(dp)) != nullptr) {
            int fd;
            if (std::isdigit(dirp->d_name[0]) &&
                (fd = strtol(dirp->d_name, nullptr, 10)) >= from_fd + 2) {
                close(fd);
            }
        }

        closedir(dp);
        return true;
    }

    /**
     * If PATH is not defined, the OS provides some default value.
     */
    static const char *default_path_env() {
        return ":/bin:/usr/bin";
    }

    static const char *get_path_env() {
        const char *s = getenv("PATH");
        return (s != nullptr) ? s : default_path_env();
    }

    static const char *const *effective_pathv() {
        const char *path = get_path_env();
        // it's safe to convert from size_t to int, :)
        int count = static_cast<int>(mpp::string_ref(path).count(':')) + 1;
        size_t pathvsize = sizeof(const char *) * (count + 1);
        size_t pathsize = strlen(path) + 1;
        const char **pathv = reinterpret_cast<const char **>(malloc(pathvsize + pathsize));

        if (pathv == nullptr) {
            return nullptr;
        }

        char *p = reinterpret_cast<char *>(pathv + pathvsize);
        memcpy(p, path, pathsize);

        // split PATH by replacing ':' with '\0'
        // and empty components with "."
        for (int i = 0; i < count; i++) {
            char *sep = p + strcspn(p, ":");
            pathv[i] = (p == sep) ? "." : p;
            *sep = '\0';
            p = sep + 1;
        }
        pathv[count] = nullptr;
        return pathv;
    }

    /**
     * Exec file as a shell script but without shebang (#!).
     * This is a historical tradeoff.
     * see GNU libc documentation.
     */
    void execve_without_shebang(const char *file, const char **argv, char **envp) {
        // Use the extra word of space provided for us in argv by caller.
        const char *argv0 = argv[0];
        const char *const *end = argv;
        while (*end != nullptr) {
            ++end;
        }
        memmove(argv + 2, argv + 1, (end - argv) * sizeof(*end));
        argv[0] = "/bin/sh";
        argv[1] = file;
        execve(argv[0], const_cast<char **>(argv), envp);

        // oops, /bin/sh can't be executed, just fall through
        memmove(argv + 1, argv + 2, (end - argv) * sizeof(*end));
        argv[0] = argv0;
    }

    /**
     * Like execve(2), but the file is always assumed to be a shell script
     * and the system default shell is invoked to run it.
     */
    void execve_or_shebang(const char *file, const char **argv, char **envp) {
        execve(file, const_cast<char **>(argv), envp);
        // or the shell doesn't provide a shebang
        if (errno == ENOEXEC) {
            execve_without_shebang(file, argv, envp);
        }
    }

    /**
     * mpp implementation of the GNU extension execvpe()
     */
    void mpp_execvpe(const char *file, const char **argv, char **envp) {
        if (envp == nullptr || envp == environ) {
            execvp(file, const_cast<char *const *>(argv));
            return;
        }

        if (*file == '\0') {
            errno = ENOENT;
            return;
        }

        if (strchr(file, '/') != nullptr) {
            execve_or_shebang(file, argv, envp);

        } else {
            // We must search PATH (parent's, not child's)
            const char *const *pathv = effective_pathv();

            // prepare the full space to avoid memory allocation
            char absolute_path[PATH_MAX] = {0};
            int filelen = strlen(file);
            int sticky_errno = 0;

            for (auto dirs = pathv; *dirs; dirs++) {
                const char *dir = *dirs;
                int dirlen = strlen(dir);
                if (filelen + dirlen + 2 >= PATH_MAX) {
                    errno = ENAMETOOLONG;
                    continue;
                }

                memcpy(absolute_path, dir, dirlen);
                if (absolute_path[dirlen - 1] != '/') {
                    absolute_path[dirlen++] = '/';
                }

                memcpy(absolute_path + dirlen, file, filelen);
                absolute_path[dirlen + filelen] = '\0';
                execve_or_shebang(absolute_path, argv, envp);

                // If permission is denied for a file (the attempted
                // execve returned EACCES), these functions will continue
                // searching the rest of the search path.  If no other
                // file is found, however, they will return with the
                // global variable errno set to EACCES.
                switch (errno) {
                    case EACCES:
                        sticky_errno = errno;
                        // fall-through
                    case ENOENT:
                    case ENOTDIR:
#ifdef ELOOP
                    case ELOOP:
#endif
#ifdef ESTALE
                    case ESTALE:
#endif
#ifdef ENODEV
                    case ENODEV:
#endif
#ifdef ETIMEDOUT
                    case ETIMEDOUT:
#endif
                        // Try other directories in PATH
                        break;
                    default:
                        return;
                }
            }

            // tell the caller the real errno
            if (sticky_errno != 0) {
                errno = sticky_errno;
            }
        }
    }

    static void child_proc(const process_startup &startup,
                           process_info &info,
                           fd_type *pstdin, fd_type *pstdout, fd_type *pstderr) {
        // in child process
        if (!startup._stdin.redirected()) {
            close_fd(pstdin[PIPE_WRITE]);
        }
        if (!startup._stdout.redirected()) {
            close_fd(pstdout[PIPE_READ]);
        }

        dup2(pstdin[PIPE_READ], STDIN_FILENO);
        dup2(pstdout[PIPE_WRITE], STDOUT_FILENO);

        /*
         * pay special attention to stderr,
         * there are 2 cases:
         *      1. redirect stderr to stdout
         *      2. redirect stderr to a file
         */
        if (startup.merge_outputs) {
            // redirect stderr to stdout
            dup2(pstdout[PIPE_WRITE], STDERR_FILENO);
        } else {
            // redirect stderr to a file
            if (!startup._stderr.redirected()) {
                close_fd(pstderr[PIPE_READ]);
            }
            dup2(pstderr[PIPE_WRITE], STDERR_FILENO);
        }

        close_fd(pstdin[PIPE_READ]);
        close_fd(pstdout[PIPE_WRITE]);
        close_fd(pstderr[PIPE_WRITE]);

        // command-line and environments
        size_t asize = startup._cmdline.size();
        size_t esize = startup._env.size();
        char *argv[asize + 1];
        char *envp[esize + 1];

        // argv and envp are always terminated with a nullptr
        argv[asize] = nullptr;
        envp[esize] = nullptr;

        // copy command-line arguments
        for (std::size_t i = 0; i < asize; ++i) {
            argv[i] = const_cast<char *>(startup._cmdline[i].c_str());
        }

        // copy environment variables
        std::vector<std::string> envs;
        std::stringstream buffer;
        for (const auto &e : startup._env) {
            buffer.str("");
            buffer.clear();
            buffer << e.first << "=" << e.second;
            envs.emplace_back(buffer.str());
        }

        for (std::size_t i = 0; i < esize; ++i) {
            envp[i] = const_cast<char *>(envs[i].c_str());
        }

        // close everything
        if (!close_all_descriptors()) {
            // try luck failed, close the old way
            int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));
            for (int fd = FAIL_FILENO + 1; fd < max_fd; fd++) {
                if (close(fd) == -1 && errno != EBADF) {
                    // oops, we cannot close this fd
                    // TODO: should we report this as an error?
                    continue;
                }
            }
        }

        // change cwd
        if (chdir(startup._cwd.c_str()) != 0) {
            mpp::throw_ex<mpp::runtime_error>("unable to change current working directory");
        }

        // run subprocess
        mpp_execvpe(argv[0], const_cast<const char **>(argv), envp);

        // TODO: report error
        _exit(1);
        mpp::throw_ex<mpp::runtime_error>("unable to exec commands in subprocess");
    }

    void create_process_impl(const process_startup &startup,
                             process_info &info,
                             fd_type *pstdin, fd_type *pstdout, fd_type *pstderr) {
        pid_t pid = fork();
        if (pid < 0) {
            mpp::throw_ex<mpp::runtime_error>("unable to fork subprocess");

        } else if (pid == 0) {
            // in child process
            child_proc(startup, info, pstdin, pstdout, pstderr);

        } else {
            // in parent process
            if (!startup._stdin.redirected()) {
                close_fd(pstdin[PIPE_READ]);
            }
            if (!startup._stdout.redirected()) {
                close_fd(pstdout[PIPE_WRITE]);
            }

            /*
             * pay special attention to stderr,
             * there are 2 cases:
             *      1. redirect stderr to stdout
             *      2. redirect stderr to a file
             */
            if (startup.merge_outputs) {
                // redirect stderr to stdout
                // do nothing
            } else {
                // redirect stderr to a file
                if (!startup._stderr.redirected()) {
                    close_fd(pstderr[PIPE_WRITE]);
                }
            }

            info._pid = pid;
            info._stdin = pstdin[PIPE_WRITE];
            info._stdout = pstdout[PIPE_READ];
            info._stderr = pstderr[PIPE_READ];

            // on *nix systems, fork() doesn't create threads to run process
            info._tid = FD_INVALID;
        }
    }

    void close_process(process_info &info) {
        mpp_impl::close_fd(info._stdin);
        mpp_impl::close_fd(info._stdout);
        mpp_impl::close_fd(info._stderr);
    }

    int wait_for(const process_info &info) {
        int status;
        while (waitpid(info._pid, &status, 0) < 0) {
            switch (errno) {
                case ECHILD:
                    return 0;
                case EINTR:
                    break;
                default:
                    return -1;
            }
        }

        if (WIFEXITED(status)) {
            // The child exited normally, get its exit code
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            // The child exited because of a signal.
            // The best value to return is 0x80 + signal number,
            // because that is what all Unix shells do, and because
            // it allows callers to distinguish between process exit and
            // oricess death by signal.
            //
            // Breaking changes happens if we are running on solaris:
            // the historical behaviour on Solaris is to return the
            // original signal number, but we will ignore that!
            return 0x80 + WTERMSIG(status);
        } else {
            // unknown exit code, pass it through
            return status;
        }
    }

    void terminate_process(const process_info &info, bool force) {
        kill(info._pid, force ? SIGKILL : SIGTERM);
    }

    bool process_exited(const process_info &info) {
        // if WNOHANG was specified and one or more child(ren)
        // specified by pid exist, but have not yet changed state,
        // then 0 is returned. On error, -1 is returned.
        int result = waitpid(info._pid, nullptr, WNOHANG);

        if (result == -1) {
            if (errno != ECHILD) {
                // when WNOHANG was set, errno could only be ECHILD
                mpp::throw_ex<mpp::runtime_error>("should not reach here");
            }

            // waitpid() cannot find the child process identified by pid,
            // there are two cases of this situation depending on signal set
            struct sigaction sa{};
            if (sigaction(SIGCHLD, nullptr, &sa) != 0) {
                // only happens when kernel bug
                mpp::throw_ex<mpp::runtime_error>("should not reach here");
            }

#if defined(__APPLE__)
            void *handler = reinterpret_cast<void *>(sa.__sigaction_u.__sa_handler);
#elif defined(__linux__)
            void *handler = reinterpret_cast<void *>(sa.sa_handler);
#endif

            if (handler == reinterpret_cast<void *>(SIG_IGN)) {
                // in this situation we cannot check whether
                // a child process has exited in normal way, because
                // the child process is not belong to us any more, and
                // the kernel will move its owner to init without notifying us.
                // so we will try the fallback method.
                std::string path = std::string("/proc/") + std::to_string(info._pid);
                struct stat buf{};

                // when /proc/<pid> doesn't exist, the process has exited.
                // there will be race conditions: our process exited and
                // another process started with the same pid.
                // to eliminate this case, we should check /proc/<pid>/cmdline
                // but it's too complex and not always reliable.
                return stat(path.c_str(), &buf) == -1 && errno == ENOENT;

            } else {
                // we didn't set SIG_IGN for SIGCHLD
                // there is only one case here theoretically:
                // the child has exited too early before we checked it.
                return true;
            }
        }

        return result == 0;
    }
}

#endif