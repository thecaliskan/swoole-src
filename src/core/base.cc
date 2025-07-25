/*
 +----------------------------------------------------------------------+
 | Swoole                                                               |
 +----------------------------------------------------------------------+
 | This source file is subject to version 2.0 of the Apache license,    |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.apache.org/licenses/LICENSE-2.0.html                      |
 | If you did not receive a copy of the Apache2.0 license and are unable|
 | to obtain it through the world-wide-web, please send a note to       |
 | license@swoole.com so we can mail you a copy immediately.            |
 +----------------------------------------------------------------------+
 | Author: Tianfeng Han  <rango@swoole.com>                             |
 +----------------------------------------------------------------------+
 */

#include "swoole.h"

#include <cstdarg>
#include <cassert>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/resource.h>

#ifdef __MACH__
#include <sys/syslimits.h>
#endif

#include <list>
#include <set>
#include <random>

#include "swoole_api.h"
#include "swoole_string.h"
#include "swoole_signal.h"
#include "swoole_memory.h"
#include "swoole_protocol.h"
#include "swoole_util.h"
#include "swoole_async.h"
#include "swoole_c_api.h"
#include "swoole_coroutine_c_api.h"
#include "swoole_coroutine_system.h"
#include "swoole_ssl.h"

#if defined(__APPLE__) && defined(HAVE_CCRANDOMGENERATEBYTES)
#include <Availability.h>
#if (defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 101000) ||                         \
    (defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && __IPHONE_OS_VERSION_MIN_REQUIRED >= 80000)
#define OPENSSL_APPLE_CRYPTO_RANDOM 1
#include <CommonCrypto/CommonCryptoError.h>
#include <CommonCrypto/CommonRandom.h>
#endif
#endif

using swoole::Logger;
using swoole::NameResolver;
using swoole::String;
using swoole::coroutine::System;

#ifdef HAVE_GETRANDOM
#include <sys/random.h>
#else
static ssize_t getrandom(void *buffer, size_t size, unsigned int __flags) {
#if defined(HAVE_CCRANDOMGENERATEBYTES)
    /*
     * arc4random_buf on macOS uses ccrng_generate internally from which
     * the potential error is silented to respect the portable arc4random_buf interface contract
     */
    if (CCRandomGenerateBytes(buffer, size) == kCCSuccess) {
        return size;
    }
    return -1;
#elif defined(HAVE_ARC4RANDOM)
    arc4random_buf(buffer, size);
    return size;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    size_t read_bytes;
    ssize_t n;
    for (read_bytes = 0; read_bytes < size; read_bytes += (size_t) n) {
        n = read(fd, (char *) buffer + read_bytes, size - read_bytes);
        if (n <= 0) {
            break;
        }
    }

    close(fd);

    return read_bytes;
#endif
}
#endif

swoole::Global SwooleG = {};
thread_local swoole::ThreadGlobal SwooleTG = {};
thread_local char sw_error[SW_ERROR_MSG_SIZE];
std::mutex sw_thread_lock;

static void swoole_fatal_error_impl(int code, const char *format, ...);

swoole::Logger *sw_logger() {
    return SwooleG.logger;
}

void *sw_malloc(size_t size) {
    return SwooleG.std_allocator.malloc(size);
}

void sw_free(void *ptr) {
    return SwooleG.std_allocator.free(ptr);
}

void *sw_calloc(size_t nmemb, size_t size) {
    return SwooleG.std_allocator.calloc(nmemb, size);
}

void *sw_realloc(void *ptr, size_t size) {
    return SwooleG.std_allocator.realloc(ptr, size);
}

static void bug_report_message_init() {
    SwooleG.bug_report_message += "\n" + std::string(SWOOLE_BUG_REPORT) + "\n";

    utsname u;
    if (uname(&u) != -1) {
        SwooleG.bug_report_message +=
            swoole::std_string::format("OS: %s %s %s %s\n", u.sysname, u.release, u.version, u.machine);
    }

#ifdef __VERSION__
    SwooleG.bug_report_message += swoole::std_string::format("GCC_VERSION: %s\n", __VERSION__);
#endif

#ifdef SW_USE_OPENSSL
    SwooleG.bug_report_message += swoole_ssl_get_version_message();
#endif
}

void swoole_init() {
    if (SwooleG.init) {
        return;
    }

    SwooleG = {};
    sw_memset_zero(sw_error, SW_ERROR_MSG_SIZE);

    SwooleG.running = 1;
    SwooleG.init = 1;
    SwooleG.enable_coroutine = 1;
    SwooleG.std_allocator = {malloc, calloc, realloc, free};
    SwooleG.stdout_ = stdout;
    SwooleG.fatal_error = swoole_fatal_error_impl;
    SwooleG.cpu_num = SW_MAX(1, sysconf(_SC_NPROCESSORS_ONLN));
    SwooleG.pagesize = getpagesize();
    SwooleG.max_file_content = SW_MAX_FILE_CONTENT;

    // DNS options
    SwooleG.dns_tries = 1;
    SwooleG.dns_resolvconf_path = SW_DNS_RESOLV_CONF;

    // get system uname
    uname(&SwooleG.uname);
    // random seed
    srandom(time(nullptr));

    if (!SwooleG.logger) {
        SwooleG.logger = new Logger();
    }

    swoole_thread_init(true);

#ifdef SW_DEBUG
    sw_logger()->set_level(0);
    SwooleG.trace_flags = 0x7fffffff;
#else
    sw_logger()->set_level(SW_LOG_INFO);
#endif

    // init global shared memory
    SwooleG.memory_pool = new swoole::GlobalMemory(SW_GLOBAL_MEMORY_PAGESIZE, true);
    SwooleG.max_sockets = SW_MAX_SOCKETS_DEFAULT;
    rlimit rlmt;
    if (getrlimit(RLIMIT_NOFILE, &rlmt) < 0) {
        swoole_sys_warning("getrlimit() failed");
    } else {
        SwooleG.max_sockets = SW_MAX((uint32_t) rlmt.rlim_cur, SW_MAX_SOCKETS_DEFAULT);
        SwooleG.max_sockets = SW_MIN((uint32_t) rlmt.rlim_cur, SW_SESSION_LIST_SIZE);
    }

    if (!swoole_set_task_tmpdir(SW_TASK_TMP_DIR)) {
        exit(4);
    }

    // init signalfd
#ifdef HAVE_SIGNALFD
    swoole_signalfd_init();
    SwooleG.enable_signalfd = 1;
#endif

    // init bug report message
    bug_report_message_init();
}

SW_EXTERN_C_BEGIN

SW_API int swoole_add_hook(enum swGlobalHookType type, swHookFunc func, int push_back) {
    assert(type <= SW_GLOBAL_HOOK_END);
    return swoole::hook_add(SwooleG.hooks, type, func, push_back);
}

SW_API void swoole_call_hook(enum swGlobalHookType type, void *arg) {
    assert(type <= SW_GLOBAL_HOOK_END);
    swoole::hook_call(SwooleG.hooks, type, arg);
}

SW_API bool swoole_isset_hook(enum swGlobalHookType type) {
    assert(type <= SW_GLOBAL_HOOK_END);
    return SwooleG.hooks[type] != nullptr;
}

SW_API const char *swoole_version(void) {
    return SWOOLE_VERSION;
}

SW_API int swoole_version_id(void) {
    return SWOOLE_VERSION_ID;
}

SW_API int swoole_api_version_id(void) {
    return SWOOLE_API_VERSION_ID;
}

SW_EXTERN_C_END

void swoole_clean() {
    SW_LOOP_N(SW_MAX_HOOK_TYPE) {
        if (SwooleG.hooks[i]) {
            auto hooks = static_cast<std::list<swoole::Callback> *>(SwooleG.hooks[i]);
            delete hooks;
        }
    }

    swoole_signal_clear();
    swoole_thread_clean(true);

    if (SwooleG.logger) {
        SwooleG.logger->close();
        delete SwooleG.logger;
    }
    if (SwooleG.memory_pool != nullptr) {
        delete SwooleG.memory_pool;
    }
    SwooleG = {};
}

SW_API void swoole_set_log_level(int level) {
    if (sw_logger()) {
        sw_logger()->set_level(level);
    }
}

SW_API void swoole_set_stdout_stream(FILE *fp) {
    SwooleG.stdout_ = fp;
}

SW_API FILE *swoole_get_stdout_stream() {
    return SwooleG.stdout_;
}

SW_API int swoole_get_log_level() {
    if (sw_logger()) {
        return sw_logger()->get_level();
    } else {
        return SW_LOG_NONE;
    }
}

SW_API void swoole_set_log_file(const char *file) {
    if (sw_logger()) {
        sw_logger()->open(file);
    }
}

SW_API void swoole_set_trace_flags(long flags) {
    SwooleG.trace_flags = flags;
}

SW_API void swoole_set_print_backtrace_on_error(bool enable) {
    SwooleG.print_backtrace_on_error = enable;
}

bool swoole_set_task_tmpdir(const std::string &dir) {
    if (dir.at(0) != '/') {
        swoole_warning("wrong absolute path '%s'", dir.c_str());
        return false;
    }

    if (access(dir.c_str(), R_OK) < 0 && !swoole_mkdir_recursive(dir)) {
        swoole_warning("create task tmp dir('%s') failed", dir.c_str());
        return false;
    }

    sw_tg_buffer()->format("%s/" SW_TASK_TMP_FILE, dir.c_str());
    SwooleG.task_tmpfile = sw_tg_buffer()->to_std_string();

    if (SwooleG.task_tmpfile.length() >= SW_TASK_TMP_PATH_SIZE) {
        swoole_warning("task tmp_dir is too large, the max size is '%d'", SW_TASK_TMP_PATH_SIZE - 1);
        return false;
    }

    return true;
}

const std::string &swoole_get_task_tmpdir() {
    return SwooleG.task_tmpfile;
}

pid_t swoole_fork_exec(const std::function<void(void)> &fn) {
    pid_t pid = fork();
    switch (pid) {
    case -1:
        return false;
    case 0:
        fn();
        exit(0);
    default:
        break;
    }
    return pid;
}

pid_t swoole_fork(int flags) {
    if (!(flags & SW_FORK_EXEC)) {
        if (swoole_coroutine_is_in()) {
            swoole_fatal_error(SW_ERROR_OPERATION_NOT_SUPPORT, "must be forked outside the coroutine");
        }
        if (SwooleTG.async_threads) {
            swoole_trace("aio_task_num=%lu, reactor=%p", SwooleTG.async_threads->task_num, sw_reactor());
            swoole_fatal_error(SW_ERROR_OPERATION_NOT_SUPPORT, "can not fork after using async-threads");
        }
    }
    if (flags & SW_FORK_PRECHECK) {
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        if (flags & SW_FORK_DAEMON) {
            return pid;
        }
        /**
         * [!!!] All timers and event loops must be cleaned up after fork
         */
        if (swoole_timer_is_available()) {
            swoole_timer_free();
        }
        if (!(flags & SW_FORK_EXEC)) {
            /**
             * Do not release the allocated memory pages.
             * The global memory will be returned to the OS upon process termination.
             */
            SwooleG.memory_pool = new swoole::GlobalMemory(SW_GLOBAL_MEMORY_PAGESIZE, true);
            // reopen log file
            sw_logger()->reopen();
            // reset eventLoop
            if (swoole_event_is_available()) {
                swoole_event_free();
                swoole_trace_log(SW_TRACE_REACTOR, "reactor has been destroyed");
            }
        } else {
            sw_logger()->close();
        }
        // reset signal handler
        swoole_signal_clear();

        if (swoole_isset_hook(SW_GLOBAL_HOOK_AFTER_FORK)) {
            swoole_call_hook(SW_GLOBAL_HOOK_AFTER_FORK, nullptr);
        }
    }

    return pid;
}

bool swoole_is_main_thread() {
    return SwooleTG.main_thread;
}

void swoole_thread_init(bool main_thread) {
    if (!SwooleTG.buffer_stack) {
        SwooleTG.buffer_stack = new String(SW_STACK_BUFFER_SIZE);
    }
    if (!main_thread) {
        swoole_signal_block_all();
    }
    SwooleTG.main_thread = main_thread;
}

void swoole_thread_clean(bool main_thread) {
    if (SwooleTG.timer) {
        swoole_timer_free();
    }
    if (SwooleTG.reactor) {
        swoole_event_free();
    }
    if (SwooleTG.buffer_stack) {
        delete SwooleTG.buffer_stack;
        SwooleTG.buffer_stack = nullptr;
    }
}

void swoole_dump_ascii(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        printf("%u ", (unsigned) data[i]);
    }
    printf("\n");
}

void swoole_dump_bin(const char *data, char type, size_t size) {
    int i;
    int type_size = swoole_type_size(type);
    if (type_size <= 0) {
        return;
    }
    int n = size / type_size;

    for (i = 0; i < n; i++) {
        printf("%ld,", (long) swoole_unpack(type, data + type_size * i));
    }
    printf("\n");
}

void swoole_dump_hex(const char *data, size_t outlen) {
    for (size_t i = 0; i < outlen; ++i) {
        if ((i & 0x0fu) == 0) {
            printf("%08zX: ", i);
        }
        printf("%02X ", data[i]);
        if (((i + 1) & 0x0fu) == 0) {
            printf("\n");
        }
    }
    printf("\n");
}

/**
 * Recursive directory creation
 */
bool swoole_mkdir_recursive(const std::string &dir) {
    char tmp[PATH_MAX];
    size_t i, len = dir.length();

    // PATH_MAX limit includes string trailing null character
    if (len + 1 > PATH_MAX) {
        swoole_error_log(SW_LOG_WARNING,
                         SW_ERROR_NAME_TOO_LONG,
                         "mkdir() failed. Path exceeds the limit of %d characters",
                         PATH_MAX - 1);
        return false;
    }
    swoole_strlcpy(tmp, dir.c_str(), PATH_MAX);

    if (dir[len - 1] != '/') {
        strcat(tmp, "/");
    }

    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (access(tmp, R_OK) != 0) {
                if (mkdir(tmp, 0755) == -1) {
                    swoole_sys_warning("mkdir('%s') failed", tmp);
                    return false;
                }
            }
            tmp[i] = '/';
        }
    }

    return true;
}

int swoole_type_size(char type) {
    switch (type) {
    case 'c':
    case 'C':
        return 1;
    case 's':
    case 'S':
    case 'n':
    case 'v':
        return 2;
    case 'l':
    case 'L':
    case 'N':
    case 'V':
        return 4;
    case 'q':
    case 'Q':
    case 'J':
    case 'P':
        return 8;
    default:
        return 0;
    }
}

char *swoole_dec2hex(ulong_t value, int base) {
    assert(base > 1 && base < 37);

    static char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buf[(sizeof(ulong_t) << 3) + 1];
    char *ptr, *end;

    end = ptr = buf + sizeof(buf) - 1;
    *ptr = '\0';

    do {
        *--ptr = digits[value % base];
        value /= base;
    } while (ptr > buf && value);

    return sw_strndup(ptr, end - ptr);
}

ulong_t swoole_hex2dec(const char *hex, size_t *parsed_bytes) {
    size_t value = 0;
    *parsed_bytes = 0;
    const char *p = hex;

    if (strncasecmp(hex, "0x", 2) == 0) {
        p += 2;
    }

    while (true) {
        char c = *p;
        if ((c >= '0') && (c <= '9')) {
            value = value * 16 + (c - '0');
        } else {
            c = toupper(c);
            if ((c >= 'A') && (c <= 'Z')) {
                value = value * 16 + (c - 'A') + 10;
            } else {
                break;
            }
        }
        p++;
    }
    *parsed_bytes = p - hex;
    return value;
}

#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif

int swoole_rand(int min, int max) {
    assert(max > min);
    int _rand = rand();
    _rand = min + (int) ((double) ((double) (max) - (min) + 1.0) * ((_rand) / ((RAND_MAX) + 1.0)));
    return _rand;
}

int swoole_system_random(int min, int max) {
    static int dev_random_fd = -1;
    unsigned random_value;

    assert(max > min);

    if (dev_random_fd == -1) {
        dev_random_fd = open("/dev/urandom", O_RDONLY);
        if (dev_random_fd < 0) {
            return swoole_rand(min, max);
        }
    }

    auto next_random_byte = (char *) &random_value;
    constexpr int bytes_to_read = sizeof(random_value);

    if (read(dev_random_fd, next_random_byte, bytes_to_read) < bytes_to_read) {
        swoole_sys_warning("read() from /dev/urandom failed");
        return SW_ERR;
    }
    return min + (random_value % (max - min + 1));
}

void swoole_redirect_stdout(int new_fd) {
    if (dup2(new_fd, STDOUT_FILENO) < 0) {
        swoole_sys_warning("dup2(STDOUT_FILENO) failed");
    }
    if (dup2(new_fd, STDERR_FILENO) < 0) {
        swoole_sys_warning("dup2(STDERR_FILENO) failed");
    }
}

void swoole_redirect_stdout(const char *file) {
    auto fd = open(file, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) {
        swoole_redirect_stdout(fd);
        close(fd);
    } else {
        swoole_sys_warning("open('%s') failed", file);
    }
}

int swoole_version_compare(const char *version1, const char *version2) {
    int result = 0;

    while (result == 0) {
        char *tail1;
        char *tail2;

        unsigned long ver1 = strtoul(version1, &tail1, 10);
        unsigned long ver2 = strtoul(version2, &tail2, 10);

        if (ver1 < ver2) {
            result = -1;
        } else if (ver1 > ver2) {
            result = +1;
        } else {
            version1 = tail1;
            version2 = tail2;
            if (*version1 == '\0' && *version2 == '\0') {
                break;
            } else if (*version1 == '\0') {
                result = -1;
            } else if (*version2 == '\0') {
                result = +1;
            } else {
                version1++;
                version2++;
            }
        }
    }
    return result;
}

/**
 * Maximum common divisor
 */
uint32_t swoole_common_divisor(uint32_t u, uint32_t v) {
    assert(u > 0);
    assert(v > 0);
    uint32_t t;
    while (u > 0) {
        if (u < v) {
            t = u;
            u = v;
            v = t;
        }
        u = u - v;
    }
    return v;
}

/**
 * The least common multiple
 */
uint32_t swoole_common_multiple(uint32_t u, uint32_t v) {
    assert(u > 0);
    assert(v > 0);

    uint32_t m_cup = u;
    uint32_t n_cup = v;
    int res = m_cup % n_cup;

    while (res != 0) {
        m_cup = n_cup;
        n_cup = res;
        res = m_cup % n_cup;
    }
    return u * v / n_cup;
}

size_t sw_snprintf(char *buf, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int retval = vsnprintf(buf, size, format, args);
    va_end(args);

    if (size == 0) {
        return retval;
    } else if (sw_unlikely(retval < 0)) {
        retval = 0;
        buf[0] = '\0';
    } else if (sw_unlikely(retval >= (int) size)) {
        retval = size - 1;
        buf[retval] = '\0';
    }
    return retval;
}

size_t sw_vsnprintf(char *buf, size_t size, const char *format, va_list args) {
    int retval = vsnprintf(buf, size, format, args);
    if (sw_unlikely(retval < 0)) {
        retval = 0;
        buf[0] = '\0';
    } else if (sw_unlikely(retval >= (int) size)) {
        retval = size - 1;
        buf[retval] = '\0';
    }
    return retval;
}

int sw_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int retval = vfprintf(SwooleG.stdout_, format, args);
    va_end(args);
    return retval;
}

int swoole_itoa(char *buf, long value) {
    long i = 0, j;
    long sign_mask;
    unsigned long nn;

    sign_mask = value >> (sizeof(long) * 8 - 1);
    nn = (value + sign_mask) ^ sign_mask;
    do {
        buf[i++] = nn % 10 + '0';
    } while (nn /= 10);

    buf[i] = '-';
    i += sign_mask & 1;
    buf[i] = '\0';

    int s_len = i;
    char swap;

    for (i = 0, j = s_len - 1; i < j; ++i, --j) {
        swap = buf[i];
        buf[i] = buf[j];
        buf[j] = swap;
    }
    buf[s_len] = 0;
    return s_len;
}

int swoole_shell_exec(const char *command, pid_t *pid, bool get_error_stream) {
    pid_t child_pid;
    int fds[2];
    if (pipe(fds) < 0) {
        return SW_ERR;
    }

    if ((child_pid = fork()) == -1) {
        swoole_sys_warning("fork() failed");
        close(fds[0]);
        close(fds[1]);
        return SW_ERR;
    }

    if (child_pid == 0) {
        close(fds[SW_PIPE_READ]);

        if (get_error_stream) {
            if (fds[SW_PIPE_WRITE] == fileno(stdout)) {
                dup2(fds[SW_PIPE_WRITE], fileno(stderr));
            } else if (fds[SW_PIPE_WRITE] == fileno(stderr)) {
                dup2(fds[SW_PIPE_WRITE], fileno(stdout));
            } else {
                dup2(fds[SW_PIPE_WRITE], fileno(stdout));
                dup2(fds[SW_PIPE_WRITE], fileno(stderr));
                close(fds[SW_PIPE_WRITE]);
            }
        } else {
            if (fds[SW_PIPE_WRITE] != fileno(stdout)) {
                dup2(fds[SW_PIPE_WRITE], fileno(stdout));
                close(fds[SW_PIPE_WRITE]);
            }
        }

        execl("/bin/sh", "sh", "-c", command, nullptr);
        exit(127);
    } else {
        *pid = child_pid;
        close(fds[SW_PIPE_WRITE]);
    }
    return fds[SW_PIPE_READ];
}

char *swoole_string_format(size_t n, const char *format, ...) {
    char *buf = (char *) sw_malloc(n);
    if (!buf) {
        return nullptr;
    }

    int ret;
    va_list va_list;
    va_start(va_list, format);
    ret = vsnprintf(buf, n, format, va_list);
    va_end(va_list);
    if (ret >= 0) {
        return buf;
    }
    sw_free(buf);
    return nullptr;
}

static constexpr char characters[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U',
    'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
    'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
};

void swoole_random_string(char *buf, size_t len) {
    size_t i = 0;
    for (; i < len; i++) {
        buf[i] = characters[swoole_rand(0, sizeof(characters) - 1)];
    }
    buf[i] = '\0';
}

void swoole_random_string(std::string &str, size_t len) {
    size_t i = 0;
    for (; i < len; i++) {
        str.append(1, characters[swoole_rand(0, sizeof(characters) - 1)]);
    }
}

uint64_t swoole_random_int() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis(0, UINT64_MAX);
    return dis(gen);
}

size_t swoole_random_bytes(char *buf, size_t size) {
    size_t read_bytes = 0;
    ssize_t n;

    while (read_bytes < size) {
        size_t amount_to_read = size - read_bytes;
        n = getrandom(buf + read_bytes, amount_to_read, 0);
        if (n == -1) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            } else {
                break;
            }
        }
        read_bytes += (size_t) n;
    }

    return read_bytes;
}

bool swoole_get_env(const char *name, int *value) {
    const char *e = getenv(name);
    if (!e) {
        return false;
    }
    *value = std::stoi(e);
    return true;
}

int swoole_get_systemd_listen_fds() {
    int ret;
    if (!swoole_get_env("LISTEN_FDS", &ret)) {
        swoole_warning("invalid LISTEN_FDS");
        return -1;
    } else if (ret >= SW_MAX_LISTEN_PORT) {
        swoole_error_log(SW_LOG_ERROR, SW_ERROR_SERVER_TOO_MANY_LISTEN_PORT, "LISTEN_FDS is too big");
        return -1;
    }
    return ret;
}

#ifdef HAVE_BOOST_STACKTRACE
#include <boost/stacktrace.hpp>
#include <iostream>
void swoole_print_backtrace(void) {
    std::cout << boost::stacktrace::stacktrace();
}
#elif defined(HAVE_EXECINFO)
#include <execinfo.h>
void swoole_print_backtrace(void) {
    int size = 16;
    void *array[16];
    int stack_num = backtrace(array, size);
    char **stacktrace = backtrace_symbols(array, stack_num);
    int i;

    for (i = 0; i < stack_num; ++i) {
        printf("%s\n", stacktrace[i]);
    }
    free(stacktrace);
}
#else
void swoole_print_backtrace(void) {}
#endif

void swoole_print_backtrace_on_error(void) {
    if (SwooleG.print_backtrace_on_error) {
        swoole_print_backtrace();
    }
}

static void swoole_fatal_error_impl(int code, const char *format, ...) {
    size_t retval = 0;
    va_list args;

    retval += sw_snprintf(sw_error, SW_ERROR_MSG_SIZE, "(ERROR %d): ", code);
    va_start(args, format);
    retval += sw_vsnprintf(sw_error + retval, SW_ERROR_MSG_SIZE - retval, format, args);
    va_end(args);
    sw_logger()->put(SW_LOG_ERROR, sw_error, retval);
    swoole_exit(1);
}

void swoole_exit(int __status) {
#ifdef SW_THREAD
    /**
     * If multiple threads call exit simultaneously, it can result in a crash.
     * Implementing locking mechanisms can prevent concurrent calls to exit.
     */
    std::unique_lock<std::mutex> _lock(sw_thread_lock);
#endif
    exit(__status);
}

namespace swoole {
//-------------------------------------------------------------------------------
size_t DataHead::dump(char *_buf, size_t _len) {
    return sw_snprintf(_buf,
                       _len,
                       "DataHead[%p]\n"
                       "{\n"
                       "    long fd = %ld;\n"
                       "    uint64_t msg_id = %" PRIu64 ";\n"
                       "    uint32_t len = %d;\n"
                       "    int16_t reactor_id = %d;\n"
                       "    uint8_t type = %d;\n"
                       "    uint8_t flags = %d;\n"
                       "    uint16_t server_fd = %d;\n"
                       "    uint16_t ext_flags = %d;\n"
                       "    double time = %f;\n"
                       "}\n",
                       this,
                       fd,
                       msg_id,
                       len,
                       reactor_id,
                       type,
                       flags,
                       server_fd,
                       ext_flags,
                       time);
}

void DataHead::print() {
    sw_tg_buffer()->length = dump(sw_tg_buffer()->str, sw_tg_buffer()->size);
    printf("%.*s", (int) sw_tg_buffer()->length, sw_tg_buffer()->str);
}

std::string dirname(const std::string &file) {
    size_t index = file.find_last_of('/');
    if (index == std::string::npos) {
        return {};
    } else if (index == 0) {
        return "/";
    }
    return file.substr(0, index);
}

int hook_add(void **hooks, int type, const Callback &func, int push_back) {
    if (hooks[type] == nullptr) {
        hooks[type] = new std::list<Callback>;
    }

    auto *l = static_cast<std::list<Callback> *>(hooks[type]);
    if (push_back) {
        l->push_back(func);
    } else {
        l->push_front(func);
    }

    return SW_OK;
}

void hook_call(void **hooks, int type, void *arg) {
    if (hooks[type] == nullptr) {
        return;
    }
    const auto *l = static_cast<std::list<Callback> *>(hooks[type]);
    for (auto &i : *l) {
        i(arg);
    }
}

/**
 * return the first file of the intersection, in order of vec1
 */
std::string intersection(const std::vector<std::string> &vec1, std::set<std::string> &vec2) {
    for (const auto &vec1_item : vec1) {
        if (vec2.find(vec1_item) != vec2.end()) {
            return vec1_item;
        }
    }

    return "";
}

double microtime() {
    timeval t;
    gettimeofday(&t, nullptr);
    return (double) t.tv_sec + ((double) t.tv_usec / 1000000);
}
};  // namespace swoole
