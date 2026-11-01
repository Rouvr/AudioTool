// ============================================================================
// AudioTool (AT) — Robust Diagnostic Logging & Crash Triage Subsystem
// ============================================================================

#include "common/Logger.h"
#include "common/AppPathsAndRegistry.h"
#include "at_version.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdarg>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace at {

namespace {

const char* level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return "UNKWN";
    }
}

const char* basename_only(const char* path) {
    if (!path) return "";
    const char* p1 = strrchr(path, '\\');
    const char* p2 = strrchr(path, '/');
    const char* p = p1 > p2 ? p1 : p2;
    return p ? p + 1 : path;
}

std::string get_current_timestamp_iso() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

std::string get_timestamp_filename() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

LONG WINAPI CrashExceptionFilter(EXCEPTION_POINTERS* pEx) {
    if (!pEx || !pEx->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = pEx->ExceptionRecord->ExceptionCode;
    void* addr = pEx->ExceptionRecord->ExceptionAddress;

    const char* code_desc = "UNKNOWN_EXCEPTION";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:         code_desc = "EXCEPTION_ACCESS_VIOLATION"; break;
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    code_desc = "EXCEPTION_ARRAY_BOUNDS_EXCEEDED"; break;
        case EXCEPTION_BREAKPOINT:               code_desc = "EXCEPTION_BREAKPOINT"; break;
        case EXCEPTION_DATATYPE_MISALIGNMENT:    code_desc = "EXCEPTION_DATATYPE_MISALIGNMENT"; break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:       code_desc = "EXCEPTION_FLT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:      code_desc = "EXCEPTION_ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:       code_desc = "EXCEPTION_INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_STACK_OVERFLOW:           code_desc = "EXCEPTION_STACK_OVERFLOW"; break;
        default: break;
    }

    char mod_info[MAX_PATH * 2] = "Unknown Module";
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &hMod) && hMod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(hMod, path, MAX_PATH);
        uintptr_t offset = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(hMod);
        std::snprintf(mod_info, sizeof(mod_info), "%s+0x%llX", basename_only(path), static_cast<unsigned long long>(offset));
    }

    char crash_buf[1024];
    if (code == EXCEPTION_ACCESS_VIOLATION && pEx->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR op = pEx->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR target_addr = pEx->ExceptionRecord->ExceptionInformation[1];
        const char* op_str = (op == 0) ? "READ" : ((op == 1) ? "WRITE" : "EXECUTE");
        std::snprintf(crash_buf, sizeof(crash_buf),
                      "CRASH! %s (0x%08X) at address %p in %s attempting %s target address %p",
                      code_desc, code, addr, mod_info, op_str, reinterpret_cast<void*>(target_addr));
    } else {
        std::snprintf(crash_buf, sizeof(crash_buf),
                      "CRASH! %s (0x%08X) at address %p in %s",
                      code_desc, code, addr, mod_info);
    }

    Logger::instance().log(LogLevel::Fatal, "CRASH", "ExceptionFilter", 0, crash_buf);

    if (pEx->ContextRecord) {
#if defined(_M_X64) || defined(__x86_64__)
        char reg_buf1[512];
        std::snprintf(reg_buf1, sizeof(reg_buf1),
                      "Registers: RIP=0x%016llX RSP=0x%016llX RBP=0x%016llX EFL=0x%08lX",
                      pEx->ContextRecord->Rip, pEx->ContextRecord->Rsp,
                      pEx->ContextRecord->Rbp, pEx->ContextRecord->EFlags);
        Logger::instance().log(LogLevel::Fatal, "CRASH", "ExceptionFilter", 0, reg_buf1);

        char reg_buf2[512];
        std::snprintf(reg_buf2, sizeof(reg_buf2),
                      "           RAX=0x%016llX RBX=0x%016llX RCX=0x%016llX RDX=0x%016llX",
                      pEx->ContextRecord->Rax, pEx->ContextRecord->Rbx,
                      pEx->ContextRecord->Rcx, pEx->ContextRecord->Rdx);
        Logger::instance().log(LogLevel::Fatal, "CRASH", "ExceptionFilter", 0, reg_buf2);

        char reg_buf3[512];
        std::snprintf(reg_buf3, sizeof(reg_buf3),
                      "           RSI=0x%016llX RDI=0x%016llX R8 =0x%016llX R9 =0x%016llX",
                      pEx->ContextRecord->Rsi, pEx->ContextRecord->Rdi,
                      pEx->ContextRecord->R8,  pEx->ContextRecord->R9);
        Logger::instance().log(LogLevel::Fatal, "CRASH", "ExceptionFilter", 0, reg_buf3);

        char reg_buf4[512];
        std::snprintf(reg_buf4, sizeof(reg_buf4),
                      "           R10=0x%016llX R11=0x%016llX R12=0x%016llX R13=0x%016llX R14=0x%016llX R15=0x%016llX",
                      pEx->ContextRecord->R10, pEx->ContextRecord->R11,
                      pEx->ContextRecord->R12, pEx->ContextRecord->R13,
                      pEx->ContextRecord->R14, pEx->ContextRecord->R15);
        Logger::instance().log(LogLevel::Fatal, "CRASH", "ExceptionFilter", 0, reg_buf4);

        // --- Stack Walk and Symbol Resolution ---
        HANDLE hProcess = GetCurrentProcess();
        HANDLE hThread = GetCurrentThread();

        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        SymInitialize(hProcess, nullptr, TRUE);

        CONTEXT ctx = *pEx->ContextRecord;
        STACKFRAME64 frame = {};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;

        Logger::instance().log(LogLevel::Fatal, "CRASH", "ExceptionFilter", 0, "=== Call Stack Backtrace ===");

        int frame_num = 0;
        char sym_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        PSYMBOL_INFO pSymbol = reinterpret_cast<PSYMBOL_INFO>(sym_buffer);
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;

        while (frame_num < 32 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread, &frame, &ctx,
                                             nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            if (frame.AddrPC.Offset == 0) break;

            char frame_line[1024];
            char mod_name[MAX_PATH] = "Unknown";
            HMODULE hFrameMod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(frame.AddrPC.Offset), &hFrameMod) && hFrameMod) {
                char full_mod[MAX_PATH] = {};
                GetModuleFileNameA(hFrameMod, full_mod, MAX_PATH);
                std::snprintf(mod_name, sizeof(mod_name), "%s", basename_only(full_mod));
            }
            uint64_t mod_base = SymGetModuleBase64(hProcess, frame.AddrPC.Offset);
            uint64_t rel_offset = mod_base ? (frame.AddrPC.Offset - mod_base) : frame.AddrPC.Offset;

            DWORD64 displacement = 0;
            BOOL has_sym = SymFromAddr(hProcess, frame.AddrPC.Offset, &displacement, pSymbol);

            IMAGEHLP_LINE64 line_info = {};
            line_info.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD line_disp = 0;
            BOOL has_line = SymGetLineFromAddr64(hProcess, frame.AddrPC.Offset, &line_disp, &line_info);

            if (has_sym && has_line) {
                std::snprintf(frame_line, sizeof(frame_line),
                              "  [%2d] 0x%016llX %s!%s (+0x%llX) [%s:%lu]",
                              frame_num, frame.AddrPC.Offset, mod_name, pSymbol->Name, displacement,
                              basename_only(line_info.FileName), line_info.LineNumber);
            } else if (has_sym) {
                std::snprintf(frame_line, sizeof(frame_line),
                              "  [%2d] 0x%016llX %s!%s (+0x%llX)",
                              frame_num, frame.AddrPC.Offset, mod_name, pSymbol->Name, displacement);
            } else {
                std::snprintf(frame_line, sizeof(frame_line),
                              "  [%2d] 0x%016llX %s+0x%llX",
                              frame_num, frame.AddrPC.Offset, mod_name, rel_offset);
            }

            Logger::instance().log(LogLevel::Fatal, "CRASH", "ExceptionFilter", 0, frame_line);
            frame_num++;
        }

        SymCleanup(hProcess);
#endif
    }

    Logger::instance().flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

struct Logger::Impl {
    std::recursive_mutex  mutex;
    std::ofstream         dated_stream;
    std::ofstream         latest_stream;
    std::filesystem::path dated_path;
    std::filesystem::path latest_path;
    LogLevel              min_level = LogLevel::Debug;
    bool                  initialized = false;
};

Logger::Logger() : impl_(new Impl()) {}

Logger::~Logger() {
    shutdown();
    delete impl_;
}

Logger& Logger::instance() {
    static Logger s_inst;
    return s_inst;
}

void Logger::set_min_level(LogLevel level) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    impl_->min_level = level;
}

LogLevel Logger::get_min_level() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return impl_->min_level;
}

std::string Logger::get_current_log_path() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return impl_->dated_path.string();
}

std::string Logger::get_latest_log_path() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    return impl_->latest_path.string();
}

bool Logger::init(uint32_t max_retained_logs, LogLevel min_level) {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (impl_->initialized) return true;

    impl_->min_level = min_level;

    try {
        auto logs_dir = AppPathsAndRegistry::get_logs_dir();
        std::filesystem::create_directories(logs_dir);

        // 1. Retention Policy: Collect and rotate old logs
        std::vector<std::filesystem::path> existing_logs;
        for (const auto& entry : std::filesystem::directory_iterator(logs_dir)) {
            if (entry.is_regular_file()) {
                std::string fname = entry.path().filename().string();
                if (fname.rfind("audiotool_", 0) == 0 && entry.path().extension() == ".log") {
                    existing_logs.push_back(entry.path());
                }
            }
        }

        // Sort ascending by filename (timestamp embedded)
        std::sort(existing_logs.begin(), existing_logs.end());

        // Keep last (max_retained_logs - 1) before creating the new one
        if (max_retained_logs > 0 && existing_logs.size() >= max_retained_logs) {
            size_t delete_count = existing_logs.size() - max_retained_logs + 1;
            for (size_t i = 0; i < delete_count; ++i) {
                std::error_code ec;
                std::filesystem::remove(existing_logs[i], ec);
            }
        }

        // 2. Open new timestamped log
        std::string ts_name = "audiotool_" + get_timestamp_filename() + ".log";
        impl_->dated_path = logs_dir / ts_name;
        impl_->dated_stream.open(impl_->dated_path, std::ios::out | std::ios::app);

        // 3. Open latest.log (truncate)
        impl_->latest_path = logs_dir / "latest.log";
        impl_->latest_stream.open(impl_->latest_path, std::ios::out | std::ios::trunc);

        impl_->initialized = true;

        // Write header
        std::ostringstream hdr;
        hdr << "====================================================================\n"
            << " AudioTool Diagnostic Log Session Started\n"
            << " Version: " << AT_VERSION_STRING << " | Build ID: " << AT_BUILD_ID << "\n"
            << " Process ID: " << GetCurrentProcessId() << " | Main Thread: 0x" << std::hex << GetCurrentThreadId() << std::dec << "\n"
            << " Log File: " << impl_->dated_path.string() << "\n"
            << " Latest Link: " << impl_->latest_path.string() << "\n"
            << "====================================================================\n";

        std::string hdr_str = hdr.str();
        if (impl_->dated_stream.is_open()) impl_->dated_stream << hdr_str;
        if (impl_->latest_stream.is_open()) impl_->latest_stream << hdr_str;
        std::cout << hdr_str;

        flush();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[LOGGER ERROR] Failed to initialize logger: " << e.what() << "\n";
        return false;
    }
}

void Logger::install_crash_handler() {
    SetUnhandledExceptionFilter(CrashExceptionFilter);
    AT_LOG_INFO("CRASH", "Installed Windows SEH unhandled exception filter for crash diagnostics.");
}

void Logger::flush() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (impl_->dated_stream.is_open()) impl_->dated_stream.flush();
    if (impl_->latest_stream.is_open()) impl_->latest_stream.flush();
    std::cout.flush();
}

void Logger::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (!impl_->initialized) return;

    if (impl_->dated_stream.is_open()) {
        impl_->dated_stream << "\n[SESSION CLOSED] Log flushed and closed cleanly.\n";
        impl_->dated_stream.close();
    }
    if (impl_->latest_stream.is_open()) {
        impl_->latest_stream << "\n[SESSION CLOSED] Log flushed and closed cleanly.\n";
        impl_->latest_stream.close();
    }
    impl_->initialized = false;
}

void Logger::log(LogLevel level, const char* subsystem, const char* file, int line, const std::string& message) {
    if (level < impl_->min_level) return;

    std::string ts = get_current_timestamp_iso();
    DWORD tid = GetCurrentThreadId();
    const char* lvl_str = level_to_string(level);
    const char* fname = basename_only(file);
    const char* sub = subsystem ? subsystem : "GENERAL";

    char header_buf[256];
    std::snprintf(header_buf, sizeof(header_buf), "[%s] [TID:0x%04X] [%s] [%-10s] [%s:%d] ",
                  ts.c_str(), tid, lvl_str, sub, fname, line);

    std::string full_line = std::string(header_buf) + message + "\n";

    std::lock_guard<std::recursive_mutex> lock(impl_->mutex);
    if (impl_->dated_stream.is_open()) {
        impl_->dated_stream << full_line;
    }
    if (impl_->latest_stream.is_open()) {
        impl_->latest_stream << full_line;
    }

    // Output to console
    std::cout << full_line;

    // Output to Windows debugger
    OutputDebugStringA(full_line.c_str());

    // Auto-flush critical levels
    if (level >= LogLevel::Warn) {
        if (impl_->dated_stream.is_open()) impl_->dated_stream.flush();
        if (impl_->latest_stream.is_open()) impl_->latest_stream.flush();
        std::cout.flush();
    }
}

void Logger::log_format(LogLevel level, const char* subsystem, const char* file, int line, const char* fmt, ...) {
    if (level < impl_->min_level) return;
    if (!fmt) return;

    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    log(level, subsystem, file, line, buf);
}

} // namespace at
