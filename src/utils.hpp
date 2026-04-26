// utils.hpp - Utility functions
#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace hymo {

// Logging
class Logger {
public:
    struct TraceContext {
        std::string step;
        std::string op;
        std::string params;
        std::string result;
        long long duration_ms = -1;
    };

    static Logger& getInstance();
    void init(bool debug, bool verbose);
    void init(bool debug, bool verbose, const char* log_path);  // YukiSU: write to file
    void init(bool debug, bool verbose, const char* log_path, bool trace_steps, bool trace_params,
              bool force_fsync);
    void log(const std::string& level, const std::string& message);
    void log_trace(const std::string& level, const std::string& message, const TraceContext& ctx);
    void rotate_logs(const char* log_path, size_t rotate_mb, int keep_files);
    void set_trace_enabled(bool trace_steps, bool trace_params);
    bool trace_steps_enabled() const;
    bool trace_params_enabled() const;
    bool debug_enabled() const;
    void install_fatal_signal_handlers();
    std::string run_id() const;

private:
    Logger() = default;
    static void handle_fatal_signal(int sig);
    void write_line(const std::string& line);
    static std::string sanitize_field(const std::string& value);
    std::string format_log_line(const std::string& level, const std::string& message,
                                const TraceContext* ctx) const;

    bool debug_ = false;
    bool verbose_ = false;
    bool trace_steps_ = false;
    bool trace_params_ = false;
    bool force_fsync_ = false;
    std::unique_ptr<std::ofstream> log_file_;
    std::string log_path_;
    std::string run_id_;
    int log_fd_ = -1;
};

#define LOG_INFO(msg) Logger::getInstance().log("INFO", msg)
#define LOG_WARN(msg) Logger::getInstance().log("WARN", msg)
#define LOG_ERROR(msg) Logger::getInstance().log("ERROR", msg)
#define LOG_DEBUG(msg) Logger::getInstance().log("DEBUG", msg)
#define LOG_VERBOSE(msg) Logger::getInstance().log("VERBOSE", msg)

std::string trace_kv(const std::initializer_list<std::pair<std::string, std::string>>& fields);
std::string trace_path(const fs::path& value);
std::string trace_bool(bool value);

class TraceScope {
public:
    TraceScope(std::string step, std::string op, std::string params = "");
    ~TraceScope();
    void set_result(std::string result);
    void fail(std::string reason);

private:
    bool enabled_ = false;
    bool failed_ = false;
    std::string step_;
    std::string op_;
    std::string params_;
    std::string result_;
    std::chrono::steady_clock::time_point start_;
};

// File system utilities
bool ensure_dir_exists(const fs::path& path);
bool is_xattr_supported(const fs::path& path);
bool lsetfilecon(const fs::path& path, const std::string& context);
std::string lgetfilecon(const fs::path& path);
bool copy_path_context(const fs::path& src, const fs::path& dst);

bool mount_tmpfs(const fs::path& target, const char* source = nullptr);
bool mount_image(const fs::path& image_path, const fs::path& target,
                 const std::string& fs_type = "ext4",
                 const std::string& options = "loop,rw,noatime");
bool repair_image(const fs::path& image_path);
bool sync_dir(const fs::path& src, const fs::path& dst);
bool has_files_recursive(const fs::path& path);
bool check_tmpfs_xattr();

// EROFS support
bool is_erofs_supported();

// KSU utilities
bool send_unmountable(const fs::path& target);
bool ksu_nuke_sysfs(const std::string& target);
int grab_ksu_fd();

// Process utilities
bool camouflage_process(const std::string& name);

// Temp directory
fs::path select_temp_dir();
bool is_safe_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror = false);
bool ensure_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror = false);
void cleanup_temp_dir(const fs::path& temp_dir, bool allow_dev_mirror = false);

}  // namespace hymo
