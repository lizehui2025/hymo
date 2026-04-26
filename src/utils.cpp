// utils.cpp - Utility functions implementation
#include "utils.hpp"
#include <fcntl.h>
#include <linux/loop.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include "defs.hpp"

extern char** environ;

namespace hymo {

namespace {

std::mutex g_log_mutex;
std::atomic<int> g_signal_log_fd{-1};
std::atomic<int> g_signal_fatal_fd{-1};
std::atomic<bool> g_signal_handlers_installed{false};
std::atomic<bool> g_signal_in_flight{false};

constexpr int kHandledSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTERM};

long long steady_ms_since(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

void append_uint(char* buf, size_t& idx, size_t cap, unsigned long long value) {
    char tmp[32];
    size_t len = 0;
    do {
        tmp[len++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value > 0 && len < sizeof(tmp));
    while (len > 0 && idx + 1 < cap) {
        buf[idx++] = tmp[--len];
    }
}

size_t append_literal(char* buf, size_t idx, size_t cap, const char* lit) {
    if (!lit) {
        return idx;
    }
    while (*lit != '\0' && idx + 1 < cap) {
        buf[idx++] = *lit++;
    }
    return idx;
}

}  // namespace

// Logger implementation
Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::init(bool debug, bool verbose) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    debug_ = debug;
    verbose_ = verbose;
}

void Logger::init(bool debug, bool verbose, const char* log_path) {
    init(debug, verbose, log_path, trace_steps_, trace_params_, force_fsync_);
}

void Logger::init(bool debug, bool verbose, const char* log_path, bool trace_steps,
                  bool trace_params, bool force_fsync) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    debug_ = debug;
    verbose_ = verbose;
    trace_steps_ = trace_steps;
    trace_params_ = trace_params;
    force_fsync_ = force_fsync;
    if (run_id_.empty()) {
        std::ostringstream oss;
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        oss << ms << "-" << getpid();
        run_id_ = oss.str();
    }

    if (log_fd_ >= 0) {
        close(log_fd_);
        log_fd_ = -1;
    }
    if (log_file_ && log_file_->is_open()) {
        log_file_->close();
    }
    log_file_.reset();
    log_path_.clear();

    if (log_path && *log_path) {
        try {
            fs::path p(log_path);
            const fs::path parent = p.parent_path();
            if (!parent.empty()) {
                ensure_dir_exists(parent);
            }
            // Always append: short-lived commands (getFeatures, getStatus, etc.) run in separate
            // processes; trunc would clear the log every time the Manager refreshes.
            log_file_ = std::make_unique<std::ofstream>(log_path, std::ios::app);
            if (!log_file_->is_open()) {
                log_file_.reset();
            } else {
                log_path_ = log_path;
                log_fd_ = open(log_path, O_WRONLY | O_APPEND | O_CLOEXEC);
            }
        } catch (...) {
            log_file_.reset();
            log_fd_ = -1;
            log_path_.clear();
        }
    }
    g_signal_log_fd.store(log_fd_);
    if (!log_path_.empty()) {
        const std::string fatal_path = std::string(FATAL_LOG_FILE);
        const int old_fatal_fd = g_signal_fatal_fd.load();
        if (old_fatal_fd >= 0) {
            close(old_fatal_fd);
        }
        g_signal_fatal_fd.store(open(fatal_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
                                     0644));
    } else {
        const int old_fatal_fd = g_signal_fatal_fd.load();
        if (old_fatal_fd >= 0) {
            close(old_fatal_fd);
            g_signal_fatal_fd.store(-1);
        }
    }
    install_fatal_signal_handlers();
}

void Logger::log(const std::string& level, const std::string& message) {
    if (level == "VERBOSE" && !verbose_)
        return;
    if (level == "DEBUG" && !debug_)
        return;

    const std::string log_line = format_log_line(level, message, nullptr);
    write_line(log_line);
}

void Logger::log_trace(const std::string& level, const std::string& message, const TraceContext& ctx) {
    if (!trace_steps_ && level == "DEBUG") {
        return;
    }
    if (level == "VERBOSE" && !verbose_) {
        return;
    }
    if (level == "DEBUG" && !debug_) {
        return;
    }
    const std::string log_line = format_log_line(level, message, &ctx);
    write_line(log_line);
}

void Logger::set_trace_enabled(bool trace_steps, bool trace_params) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    trace_steps_ = trace_steps;
    trace_params_ = trace_params;
}

bool Logger::trace_steps_enabled() const {
    return trace_steps_;
}

bool Logger::trace_params_enabled() const {
    return trace_params_;
}

bool Logger::debug_enabled() const {
    return debug_;
}

std::string Logger::run_id() const {
    return run_id_;
}

std::string Logger::sanitize_field(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '|':
            out += "\\|";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::string Logger::format_log_line(const std::string& level, const std::string& message,
                                    const TraceContext* ctx) const {
    auto now = std::time(nullptr);
    char time_buf[64];
    std::tm tm_now {};
    localtime_r(&now, &tm_now);
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_now);

    std::ostringstream oss;
    const auto tid_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    oss << "[" << time_buf << "] "
        << "[" << level << "] " << sanitize_field(message) << " | run_id=" << sanitize_field(run_id_)
        << " pid=" << getpid() << " tid=" << tid_hash;
    if (ctx) {
        if (!ctx->step.empty()) {
            oss << " step=" << sanitize_field(ctx->step);
        }
        if (!ctx->op.empty()) {
            oss << " op=" << sanitize_field(ctx->op);
        }
        if (!ctx->params.empty() && trace_params_) {
            oss << " params=" << sanitize_field(ctx->params);
        }
        if (!ctx->result.empty()) {
            oss << " result=" << sanitize_field(ctx->result);
        }
        if (ctx->duration_ms >= 0) {
            oss << " duration_ms=" << ctx->duration_ms;
        }
    }
    oss << "\n";
    return oss.str();
}

void Logger::write_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (log_file_ && log_file_->is_open()) {
        *log_file_ << line;
        log_file_->flush();
        if (log_fd_ >= 0 && force_fsync_) {
            fsync(log_fd_);
        }
    } else if (log_fd_ >= 0) {
        (void)write(log_fd_, line.c_str(), line.size());
        if (force_fsync_) {
            fsync(log_fd_);
        }
    } else {
        std::clog << line;
        std::clog.flush();
    }
}

void Logger::rotate_logs(const char* log_path, size_t rotate_mb, int keep_files) {
    if (!log_path || *log_path == '\0' || keep_files < 1 || rotate_mb == 0) {
        return;
    }
    try {
        const fs::path base(log_path);
        if (!fs::exists(base) || !fs::is_regular_file(base)) {
            return;
        }
        const uintmax_t limit = rotate_mb * 1024ULL * 1024ULL;
        const uintmax_t current_size = fs::file_size(base);
        if (current_size <= limit) {
            return;
        }
        for (int i = keep_files; i >= 1; --i) {
            const fs::path from = (i == 1) ? base : fs::path(base.string() + "." + std::to_string(i - 1));
            const fs::path to = fs::path(base.string() + "." + std::to_string(i));
            if (!fs::exists(from)) {
                continue;
            }
            std::error_code ec;
            fs::remove(to, ec);
            ec.clear();
            fs::rename(from, to, ec);
        }
    } catch (...) {
    }
}

void Logger::handle_fatal_signal(int sig) {
    if (g_signal_in_flight.exchange(true)) {
        _exit(128 + sig);
    }

    char buf[256];
    size_t idx = 0;
    idx = append_literal(buf, idx, sizeof(buf), "[FATAL] signal=");
    append_uint(buf, idx, sizeof(buf), static_cast<unsigned long long>(sig));
    idx = append_literal(buf, idx, sizeof(buf), " pid=");
    append_uint(buf, idx, sizeof(buf), static_cast<unsigned long long>(getpid()));
    idx = append_literal(buf, idx, sizeof(buf), "\n");

    const int log_fd = g_signal_log_fd.load();
    const int fatal_fd = g_signal_fatal_fd.load();
    if (log_fd >= 0) {
        (void)write(log_fd, buf, idx);
        fsync(log_fd);
    }
    if (fatal_fd >= 0) {
        (void)write(fatal_fd, buf, idx);
        fsync(fatal_fd);
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

void Logger::install_fatal_signal_handlers() {
    if (g_signal_handlers_installed.exchange(true)) {
        return;
    }
    struct sigaction sa {};
    sa.sa_handler = &Logger::handle_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (int sig : kHandledSignals) {
        sigaction(sig, &sa, nullptr);
    }
}

std::string trace_kv(const std::initializer_list<std::pair<std::string, std::string>>& fields) {
    auto sanitize = [](const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            switch (c) {
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '|':
                out += "\\|";
                break;
            default:
                out.push_back(c);
                break;
            }
        }
        return out;
    };
    std::ostringstream oss;
    bool first = true;
    for (const auto& kv : fields) {
        if (!first) {
            oss << ",";
        }
        first = false;
        oss << kv.first << "=" << sanitize(kv.second);
    }
    return oss.str();
}

std::string trace_path(const fs::path& value) {
    return value.string();
}

std::string trace_bool(bool value) {
    return value ? "true" : "false";
}

TraceScope::TraceScope(std::string step, std::string op, std::string params)
    : enabled_(Logger::getInstance().trace_steps_enabled()),
      step_(std::move(step)),
      op_(std::move(op)),
      params_(std::move(params)),
      start_(std::chrono::steady_clock::now()) {
    if (!enabled_) {
        return;
    }
    Logger::TraceContext ctx;
    ctx.step = step_;
    ctx.op = op_;
    ctx.params = params_;
    ctx.result = "BEGIN";
    Logger::getInstance().log_trace("DEBUG", "TRACE", ctx);
}

TraceScope::~TraceScope() {
    if (!enabled_) {
        return;
    }
    Logger::TraceContext ctx;
    ctx.step = step_;
    ctx.op = op_;
    ctx.params = params_;
    ctx.result = failed_ ? ("FAIL:" + result_) : (result_.empty() ? "END" : result_);
    ctx.duration_ms = steady_ms_since(start_);
    Logger::getInstance().log_trace(failed_ ? "ERROR" : "DEBUG", "TRACE", ctx);
}

void TraceScope::set_result(std::string result) {
    result_ = std::move(result);
}

void TraceScope::fail(std::string reason) {
    failed_ = true;
    result_ = std::move(reason);
}

// File system utilities
bool ensure_dir_exists(const fs::path& path) {
    try {
        if (!fs::exists(path)) {
            fs::create_directories(path);
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create directory " + path.string() + ": " + e.what());
        return false;
    }
}

bool lsetfilecon(const fs::path& path, const std::string& context) {
#ifdef __ANDROID__
    if (lsetxattr(path.c_str(), SELINUX_XATTR, context.c_str(), context.length(), 0) == 0) {
        return true;
    }
    LOG_DEBUG("lsetfilecon failed for " + path.string() + ": " + strerror(errno));
#endif  // #ifdef __ANDROID__
    return false;
}

std::string lgetfilecon(const fs::path& path) {
#ifdef __ANDROID__
    char buf[256];
    ssize_t len = lgetxattr(path.c_str(), SELINUX_XATTR, buf, sizeof(buf));
    if (len > 0) {
        return std::string(buf, len);
    }
#endif  // #ifdef __ANDROID__
    return DEFAULT_SELINUX_CONTEXT;
}

bool copy_path_context(const fs::path& src, const fs::path& dst) {
    if (fs::exists(src) ){
        return lsetfilecon(dst, lgetfilecon(src)); 
    }
    return false;
}

bool is_xattr_supported(const fs::path& path) {
    auto test_file = path / ".xattr_test";
    try {
        std::ofstream f(test_file);
        f << "test";
        f.close();

        bool supported = lsetfilecon(test_file, DEFAULT_SELINUX_CONTEXT);
        fs::remove(test_file);
        return supported;
    } catch (...) {
        return false;
    }
}

bool mount_tmpfs(const fs::path& target, const char* source) {
    if (!ensure_dir_exists(target)) {
        return false;
    }

    const char* src = (source && *source) ? source : OVERLAY_SOURCE;
    if (mount(src, target.c_str(), "tmpfs", 0, "mode=0755") != 0) {
        LOG_ERROR("Failed to mount tmpfs at " + target.string() + ": " + strerror(errno));
        return false;
    }

    return true;
}

bool has_files_recursive(const fs::path& path) {
    if (!fs::exists(path) || !fs::is_directory(path)) {
        return false;
    }

    try {
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (fs::is_regular_file(entry) || fs::is_symlink(entry)) {
                return true;
            }
        }
    } catch (...) {
        return true;
    }

    return false;
}

// Forward declaration for loop device helper
static int setup_loop_device(const std::string& image_path, std::string& loop_path, bool read_only);

// EROFS support - check if kernel supports EROFS filesystem
bool is_erofs_supported() {
    std::ifstream fs("/proc/filesystems");
    if (!fs.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(fs, line)) {
        if (line.find("erofs") != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Loop device helpers
static int setup_loop_device(const std::string& image_path, std::string& loop_path,
                             bool read_only) {
    int control_fd = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (control_fd < 0) {
        LOG_ERROR("Failed to open /dev/loop-control: " + std::string(strerror(errno)));
        return -1;
    }

    int loop_nr = ioctl(control_fd, LOOP_CTL_GET_FREE);
    close(control_fd);

    if (loop_nr < 0) {
        LOG_ERROR("Failed to allocate loop device");
        return -1;
    }

    loop_path = "/dev/block/loop" + std::to_string(loop_nr);
    if (access(loop_path.c_str(), F_OK) != 0) {
        struct stat st;
        // Check if /dev/block directory exists, if not fallback to /dev/loop
        if (stat("/dev/block", &st) == 0 && S_ISDIR(st.st_mode)) {
            // Maybe we need to mknod? try /dev/loop first as it is more standard
            loop_path = "/dev/loop" + std::to_string(loop_nr);
        } else {
            loop_path = "/dev/loop" + std::to_string(loop_nr);
        }
    }

    // Double check accessibility
    if (access(loop_path.c_str(), F_OK) != 0) {
        // Try opposite convention
        if (loop_path.find("/dev/block/") != std::string::npos)
            loop_path = "/dev/loop" + std::to_string(loop_nr);
        else
            loop_path = "/dev/block/loop" + std::to_string(loop_nr);
    }

    int loop_fd = open(loop_path.c_str(), O_RDWR | O_CLOEXEC);
    if (loop_fd < 0) {
        LOG_ERROR("Failed to open loop device " + loop_path + ": " + strerror(errno));
        return -1;
    }

    int file_fd = open(image_path.c_str(), read_only ? O_RDONLY : O_RDWR | O_CLOEXEC);
    if (file_fd < 0) {
        LOG_ERROR("Failed to open image " + image_path + ": " + strerror(errno));
        close(loop_fd);
        return -1;
    }

    if (ioctl(loop_fd, LOOP_SET_FD, file_fd) < 0) {
        LOG_ERROR("Failed to bind loop device: " + std::string(strerror(errno)));
        close(file_fd);
        close(loop_fd);
        return -1;
    }
    close(file_fd);

    struct loop_info64 info;
    memset(&info, 0, sizeof(info));
    info.lo_flags = LO_FLAGS_AUTOCLEAR;
    if (read_only)
        info.lo_flags |= LO_FLAGS_READ_ONLY;

    if (ioctl(loop_fd, LOOP_SET_STATUS64, &info) < 0) {
        LOG_ERROR("Failed to set loop status: " + std::string(strerror(errno)));
        ioctl(loop_fd, LOOP_CLR_FD, 0);
        close(loop_fd);
        return -1;
    }

    return loop_fd;
}

bool mount_image(const fs::path& image_path, const fs::path& target, const std::string& fs_type,
                 const std::string& options) {
    if (!ensure_dir_exists(target)) {
        return false;
    }

    unsigned long flags = 0;
    std::string data;
    bool read_only = false;
    bool remount = false;
    bool bind = false;

    std::stringstream ss(options);
    std::string segment;
    while (std::getline(ss, segment, ',')) {
        if (segment == "loop")
            continue;
        else if (segment == "rw")
            read_only = false;
        else if (segment == "ro") {
            flags |= MS_RDONLY;
            read_only = true;
        } else if (segment == "noatime")
            flags |= MS_NOATIME;
        else if (segment == "noexec")
            flags |= MS_NOEXEC;
        else if (segment == "nosuid")
            flags |= MS_NOSUID;
        else if (segment == "nodev")
            flags |= MS_NODEV;
        else if (segment == "sync")
            flags |= MS_SYNCHRONOUS;
        else if (segment == "bind") {
            flags |= MS_BIND;
            bind = true;
        } else if (segment == "remount") {
            flags |= MS_REMOUNT;
            remount = true;
        } else {
            if (!data.empty())
                data += ",";
            data += segment;
        }
    }

    std::string source;
    int loop_fd = -1;

    // Determine safe source
    if (bind || remount) {
        source = image_path.string();
    } else if (fs::is_regular_file(image_path)) {
        // Setup loop device for file
        source = "";
        loop_fd = setup_loop_device(image_path.string(), source, read_only);
        if (loop_fd < 0)
            return false;
    } else {
        source = image_path.string();
    }

    int ret = mount(source.c_str(), target.c_str(), fs_type.c_str(), flags, data.c_str());

    if (ret != 0) {
        LOG_ERROR("mount failed: " + std::string(strerror(errno)) + " (src=" + source +
                  ", tgt=" + target.string() + ", type=" + fs_type + ")");
        if (loop_fd >= 0) {
            // If mount failed, explicitly clear loop device to avoid leaking it
            // even though AUTOCLEAR is set, it might need fd close to trigger
            ioctl(loop_fd, LOOP_CLR_FD, 0);
            close(loop_fd);
        }
        return false;
    }

    if (loop_fd >= 0)
        close(loop_fd);

    return true;
}

// Run external binary via execve (no shell)
static bool exec_run(const char* bin_path, const std::vector<const char*>& argv) {
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("fork failed: " + std::string(strerror(errno)));
        return false;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
                close(devnull);
        }
        execve(bin_path, const_cast<char* const*>(argv.data()), environ);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) {
        LOG_ERROR("waitpid failed");
        return false;
    }
    return WIFEXITED(status) && (WEXITSTATUS(status) <= 2);
}

bool repair_image(const fs::path& image_path) {
    LOG_INFO("Running e2fsck on " + image_path.string());

    const char* e2fsck_paths[] = {"/system/bin/e2fsck", "/sbin/e2fsck", "/vendor/bin/e2fsck"};
    const char* e2fsck_bin = nullptr;
    for (const auto& p : e2fsck_paths) {
        if (access(p, X_OK) == 0) {
            e2fsck_bin = p;
            break;
        }
    }
    if (!e2fsck_bin) {
        LOG_ERROR("e2fsck not found");
        return false;
    }

    std::string path_str = image_path.string();
    std::vector<const char*> argv = {e2fsck_bin, "-y", "-f", path_str.c_str(), nullptr};

    if (!exec_run(e2fsck_bin, argv)) {
        LOG_ERROR("e2fsck failed");
        return false;
    }
    LOG_INFO("Image repair success");
    return true;
}

static bool native_cp_r(const fs::path& src, const fs::path& dst) {
    try {
        LOG_DEBUG("native_cp_r: " + src.string() + " -> " + dst.string());

        if (!fs::exists(dst)) {
            fs::create_directories(dst);
            fs::permissions(dst, fs::status(src).permissions());
            copy_path_context(src, dst);
        }

        int count = 0;
        for (const auto& entry : fs::directory_iterator(src)) {
            auto dst_path = dst / entry.path().filename();
            count++;

            if (fs::is_directory(entry)) {
                if (!native_cp_r(entry.path(), dst_path)) {
                    LOG_ERROR("Failed to copy dir: " + entry.path().string());
                    return false;
                }
            } else if (fs::is_symlink(entry)) {
                auto link_target = fs::read_symlink(entry.path());
                if (fs::exists(dst_path)) {
                    fs::remove(dst_path);
                }
                fs::create_symlink(link_target, dst_path);
                copy_path_context(entry.path(), dst_path);
            } else {
                fs::copy_file(entry.path(), dst_path, fs::copy_options::overwrite_existing);
                fs::permissions(dst_path, fs::status(entry.path()).permissions());
                copy_path_context(entry.path(), dst_path);
            }
        }

        LOG_DEBUG("Copied " + std::to_string(count) + " items from " + src.string());
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("native_cp_r failed (" + src.string() + " -> " + dst.string() +
                  "): " + std::string(e.what()));
        return false;
    }
}

bool sync_dir(const fs::path& src, const fs::path& dst) {
    LOG_DEBUG("sync_dir: " + src.string() + " -> " + dst.string());

    if (!fs::exists(src)) {
        LOG_WARN("sync_dir: source does not exist: " + src.string());
        return true;  // Not an error if source doesn't exist
    }

    if (!ensure_dir_exists(dst)) {
        LOG_ERROR("sync_dir: failed to create dst: " + dst.string());
        return false;
    }

    bool result = native_cp_r(src, dst);
    LOG_DEBUG("sync_dir result: " + std::to_string(result));
    return result;
}

// Check if tmpfs supports xattr on this device
bool check_tmpfs_xattr() {
    fs::path temp_dir = select_temp_dir() / "xattr_check";
    if (!mount_tmpfs(temp_dir)) {
        return false;
    }
    bool supported = is_xattr_supported(temp_dir);
    umount2(temp_dir.c_str(), MNT_DETACH);
    rmdir(temp_dir.c_str());
    return supported;
}

// Process utilities
bool camouflage_process(const std::string& name) {
    if (prctl(PR_SET_NAME, name.c_str(), 0, 0, 0) == 0) {
        return true;
    }
    LOG_WARN("Failed to camouflage process: " + std::string(strerror(errno)));
    return false;
}

fs::path select_temp_dir() {
    fs::path run_dir(RUN_DIR);
    ensure_dir_exists(run_dir);
    return run_dir / "workdir";
}

static std::string normalize_path_string(const fs::path& path) {
    std::string normalized = path.lexically_normal().string();
    if (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

static bool is_dangerous_temp_path(const fs::path& path, bool allow_dev_mirror) {
    std::string p = normalize_path_string(path);
    if (p.empty() || p == "." || p == "..") {
        return true;
    }

    if (p == "/" || p == "/data" || p == "/data/adb" || p == "/data/adb/hymo") {
        return true;
    }

    if (allow_dev_mirror && (p == "/dev/hymo_mirror" || p.rfind("/dev/hymo_mirror/", 0) == 0)) {
        return false;
    }

    if (p.rfind("/dev", 0) == 0 || p.rfind("/proc", 0) == 0 || p.rfind("/sys", 0) == 0) {
        return true;
    }

    return false;
}

bool is_safe_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror) {
    return !is_dangerous_temp_path(temp_dir, allow_dev_mirror);
}

bool ensure_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror) {
    if (!is_safe_temp_dir(temp_dir, allow_dev_mirror)) {
        LOG_ERROR("Refusing to clean unsafe temp dir: " + temp_dir.string());
        return false;
    }

    try {
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
        fs::create_directories(temp_dir);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to prepare temp dir " + temp_dir.string() + ": " + e.what());
        return false;
    }
}

void cleanup_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror) {
    if (!is_safe_temp_dir(temp_dir, allow_dev_mirror)) {
        LOG_WARN("Skipping cleanup for unsafe temp dir: " + temp_dir.string());
        return;
    }

    try {
        if (fs::exists(temp_dir)) {
            fs::remove_all(temp_dir);
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to clean up temp dir " + temp_dir.string() + ": " + e.what());
    }
}

// KSU utilities
static int ksu_fd = -1;
static bool ksu_checked = false;

int grab_ksu_fd() {
    if (!ksu_checked) {
        syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &ksu_fd);
        ksu_checked = true;
    }
    return ksu_fd;
}

#ifdef __ANDROID__
struct KsuAddTryUmount {
    uint64_t arg;
    uint32_t flags;
    uint8_t mode;
};

struct NukeExt4SysfsCmd {
    uint64_t arg;
};
#endif  // #ifdef __ANDROID__

bool send_unmountable(const fs::path& target) {
#ifdef __ANDROID__
    static std::set<std::string> sent_unmounts;

    std::string path_str = target.string();
    if (path_str.empty())
        return true;

    // Dedup check
    if (sent_unmounts.find(path_str) != sent_unmounts.end()) {
        return true;
    }

    int fd = grab_ksu_fd();
    if (fd < 0) {
        return false;
    }

    KsuAddTryUmount cmd = {
        .arg = reinterpret_cast<uint64_t>(path_str.c_str()), .flags = 2, .mode = 1};

    if (ioctl(fd, KSU_IOCTL_ADD_TRY_UMOUNT, &cmd) == 0) {
        sent_unmounts.insert(path_str);
        LOG_DEBUG("Registered unmountable path: " + path_str);
    } else {
        LOG_WARN("Failed to register unmountable path: " + path_str);
    }
#endif  // #ifdef __ANDROID__
    return true;
}

bool ksu_nuke_sysfs(const std::string& target) {
#ifdef __ANDROID__
    int fd = grab_ksu_fd();
    if (fd < 0) {
        LOG_ERROR("KSU driver not available");
        return false;
    }

    NukeExt4SysfsCmd cmd = {.arg = reinterpret_cast<uint64_t>(target.c_str())};

    if (ioctl(fd, KSU_IOCTL_NUKE_EXT4_SYSFS, &cmd) != 0) {
        LOG_ERROR("KSU nuke ioctl failed: " + std::string(strerror(errno)));
        return false;
    }

    return true;
#else
    return false;
#endif  // #ifdef __ANDROID__
}

}  // namespace hymo
