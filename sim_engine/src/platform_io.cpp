#include "platform_io.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nsim {
namespace {

std::atomic<uint64_t> g_temp_seq{0};

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        needed = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (needed <= 0) return {};
        std::wstring w(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), needed);
        return w;
    }
    std::wstring w(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), needed);
    return w;
}

struct ScopedWinHttpHandle {
    HINTERNET h = nullptr;
    ScopedWinHttpHandle() = default;
    explicit ScopedWinHttpHandle(HINTERNET handle) : h(handle) {}
    ~ScopedWinHttpHandle() {
        if (h) WinHttpCloseHandle(h);
    }
    ScopedWinHttpHandle(const ScopedWinHttpHandle &) = delete;
    ScopedWinHttpHandle &operator=(const ScopedWinHttpHandle &) = delete;
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};
#else
std::string shell_quote_posix(const std::string &value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += '\'';
    return quoted;
}
#endif

} // namespace

FileHandle open_read_only(const std::string &path) {
#ifdef _WIN32
    const std::wstring wpath = utf8_to_wide(path);
    if (wpath.empty()) return kInvalidFileHandle;
    // FILE_FLAG_OVERLAPPED enables lock-free concurrent positional reads across worker threads.
    HANDLE h = CreateFileW(
        wpath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS,
        nullptr
    );
    return (h == INVALID_HANDLE_VALUE) ? kInvalidFileHandle : reinterpret_cast<FileHandle>(h);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    return (fd < 0) ? kInvalidFileHandle : static_cast<FileHandle>(fd);
#endif
}

void close_file(FileHandle &handle) {
    if (handle == kInvalidFileHandle) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
    handle = kInvalidFileHandle;
}

bool read_at_offset(FileHandle handle, void *dst, size_t size, uint64_t offset) {
    if (handle == kInvalidFileHandle || (!dst && size > 0)) return false;
    if (size == 0) return true;
#ifdef _WIN32
    struct ThreadEvent {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~ThreadEvent() {
            if (ev) CloseHandle(ev);
        }
    };
    thread_local ThreadEvent tls_ev;
    if (!tls_ev.ev) return false;

    HANDLE h = reinterpret_cast<HANDLE>(handle);
    auto *out = static_cast<uint8_t *>(dst);
    size_t remaining = size;
    uint64_t pos = offset;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 32u * 1024u * 1024u));
        ResetEvent(tls_ev.ev);
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(pos & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>((pos >> 32) & 0xFFFFFFFFull);
        ov.hEvent = tls_ev.ev;
        DWORD got = 0;
        if (!ReadFile(h, out, chunk, &got, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (!GetOverlappedResult(h, &ov, &got, TRUE)) return false;
        }
        if (got == 0) return false;
        out += got;
        pos += got;
        remaining -= got;
    }
    return true;
#else
    const int fd = static_cast<int>(handle);
    auto *out = static_cast<uint8_t *>(dst);
    size_t remaining = size;
    uint64_t pos = offset;
    while (remaining > 0) {
        const ssize_t got = ::pread(fd, out, remaining, static_cast<off_t>(pos));
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        out += got;
        pos += static_cast<uint64_t>(got);
        remaining -= static_cast<size_t>(got);
    }
    return true;
#endif
}

bool get_file_size(FileHandle handle, uint64_t &out_size) {
    if (handle == kInvalidFileHandle) return false;
#ifdef _WIN32
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(reinterpret_cast<HANDLE>(handle), &li) || li.QuadPart < 0) return false;
    out_size = static_cast<uint64_t>(li.QuadPart);
    return true;
#else
    struct stat info{};
    if (::fstat(static_cast<int>(handle), &info) != 0 || info.st_size < 0) return false;
    out_size = static_cast<uint64_t>(info.st_size);
    return true;
#endif
}

bool file_larger_than(const std::string &path, uint64_t min_bytes) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::filesystem::path(path), ec);
    return !ec && sz > min_bytes;
}

bool read_entire_file_checked(const std::string &path, uint64_t min_bytes, std::vector<uint8_t> &out) {
    const FileHandle h = open_read_only(path);
    if (h == kInvalidFileHandle) return false;
    uint64_t sz = 0;
    bool ok = get_file_size(h, sz) && sz > min_bytes && sz <= 64ull * 1024ull * 1024ull;
    if (ok) {
        out.resize(static_cast<size_t>(sz));
        ok = read_at_offset(h, out.data(), out.size(), 0);
    }
    FileHandle mutable_h = h;
    close_file(mutable_h);
    if (!ok) out.clear();
    return ok;
}

std::string create_unique_temp_file(const std::filesystem::path &base_path) {
#ifdef _WIN32
    const DWORD pid = GetCurrentProcessId();
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint64_t seq = g_temp_seq.fetch_add(1, std::memory_order_relaxed);
        const std::string candidate = base_path.string() + ".tmp." + std::to_string(pid) + "." + std::to_string(seq);
        const std::wstring wcand = utf8_to_wide(candidate);
        HANDLE h = CreateFileW(wcand.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return candidate;
        }
    }
    return {};
#else
    auto pattern = base_path.string() + ".tmp.XXXXXX";
    const int fd = ::mkstemp(pattern.data());
    if (fd < 0) return {};
    ::close(fd);
    return pattern;
#endif
}

std::filesystem::path create_unique_temp_dir(const char *prefix) {
#ifdef _WIN32
    std::string stem = prefix ? prefix : "opensky-tmp";
    const std::string marker = "-XXXXXX";
    if (stem.size() >= marker.size() && stem.compare(stem.size() - marker.size(), marker.size(), marker) == 0) {
        stem.erase(stem.size() - marker.size());
    }
    const DWORD pid = GetCurrentProcessId();
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    if (ec) return {};
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint64_t seq = g_temp_seq.fetch_add(1, std::memory_order_relaxed);
        const auto cand = base / (stem + "-" + std::to_string(pid) + "-" + std::to_string(seq));
        if (std::filesystem::create_directory(cand, ec) && !ec) {
            return cand;
        }
        ec.clear();
    }
    return {};
#else
    auto pattern = (std::filesystem::temp_directory_path() / (prefix ? prefix : "opensky-XXXXXX")).string();
    if (::mkdtemp(pattern.data())) return pattern;
    return {};
#endif
}

bool atomic_replace_file(const std::filesystem::path &tmp_path, const std::filesystem::path &dest_path) {
#ifdef _WIN32
    const std::wstring wtmp = utf8_to_wide(tmp_path.string());
    const std::wstring wdst = utf8_to_wide(dest_path.string());
    if (!wtmp.empty() && !wdst.empty() &&
        MoveFileExW(wtmp.c_str(), wdst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(tmp_path, dest_path, ec);
    return !ec;
#endif
}

bool http_get_to_string(const std::string &url, const char *user_agent,
                        int connect_timeout_ms, int total_timeout_ms,
                        std::string &out_body) {
    out_body.clear();
#ifdef _WIN32
    const std::wstring wurl = utf8_to_wide(url);
    const std::wstring wua = utf8_to_wide(user_agent ? user_agent : "OpenSky-VFR/0.3");
    if (wurl.empty()) return false;

    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc) || uc.dwHostNameLength == 0) {
        return false;
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path_query = (uc.dwUrlPathLength > 0)
        ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength)
        : std::wstring(L"/");
    if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo) {
        path_query.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }

    ScopedWinHttpHandle session(WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        session.h = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session) return false;
    WinHttpSetTimeouts(session, connect_timeout_ms, connect_timeout_ms, total_timeout_ms, total_timeout_ms);

    ScopedWinHttpHandle connect(WinHttpConnect(session, host.c_str(), uc.nPort, 0));
    if (!connect) return false;

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    ScopedWinHttpHandle request(WinHttpOpenRequest(connect, L"GET", path_query.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) return false;

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        return false;
    }

    DWORD status = 0;
    DWORD status_sz = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        return false;
    }

    constexpr size_t kMaxBodyBytes = 16u * 1024u * 1024u;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) return false;
        if (avail == 0) break;
        if (out_body.size() + avail > kMaxBodyBytes) return false;
        const size_t prev = out_body.size();
        out_body.resize(prev + avail);
        DWORD got = 0;
        if (!WinHttpReadData(request, out_body.data() + prev, avail, &got)) return false;
        out_body.resize(prev + got);
        if (got == 0) break;
    }
    return !out_body.empty();
#else
    const int conn_s = std::max(1, (connect_timeout_ms + 999) / 1000);
    const int max_s = std::max(conn_s, (total_timeout_ms + 999) / 1000);
    const std::string cmd = "curl -sS -f -L --connect-timeout " + std::to_string(conn_s) +
        " --max-time " + std::to_string(max_s) + " -A " +
        shell_quote_posix(user_agent ? user_agent : "OpenSky-VFR/0.3") + " " +
        shell_quote_posix(url) + " 2>/dev/null";
    FILE *pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return false;
    char buf[4096];
    while (true) {
        const size_t got = std::fread(buf, 1, sizeof(buf), pipe);
        if (got > 0) out_body.append(buf, got);
        if (got < sizeof(buf)) break;
    }
    const int status = ::pclose(pipe);
    return status == 0 && !out_body.empty();
#endif
}

bool http_download_to_file(const std::string &url, const char *user_agent,
                           int connect_timeout_ms, int total_timeout_ms,
                           const std::string &dest_path) {
#ifdef _WIN32
    std::string body;
    if (!http_get_to_string(url, user_agent, connect_timeout_ms, total_timeout_ms, body)) return false;
    std::ofstream out(dest_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(out);
#else
    const int conn_s = std::max(1, (connect_timeout_ms + 999) / 1000);
    const int max_s = std::max(conn_s, (total_timeout_ms + 999) / 1000);
    const std::string cmd = "curl -sS -f -L --connect-timeout " + std::to_string(conn_s) +
        " --max-time " + std::to_string(max_s) + " -A " +
        shell_quote_posix(user_agent ? user_agent : "OpenSky-VFR/0.3") + " -o " +
        shell_quote_posix(dest_path) + " " + shell_quote_posix(url) + " 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
#endif
}

void launch_world_editor() {
#ifdef _WIN32
    if (!std::filesystem::exists("builder/build_region_gui.py")) return;
    auto try_spawn = [](const wchar_t *exe) {
        std::wstring cmd = std::wstring(exe) + L" builder\\build_region_gui.py";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
        return false;
    };
    if (!try_spawn(L"pythonw.exe")) {
        try_spawn(L"python.exe");
    }
#else
    const int rc = std::system("bash ./run_builder.sh >/dev/null 2>&1 &");
    (void)rc;
#endif
}

void show_fatal_error(const std::string &title, const std::string &message) {
    std::fprintf(stderr, "[FATAL] %s: %s\n", title.c_str(), message.c_str());
#ifdef _WIN32
    MessageBoxA(nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
}

void resolve_windows_working_root() {
#ifdef _WIN32
    std::error_code ec;
    auto is_root = [&](const std::filesystem::path &p) {
        return std::filesystem::is_directory(p / "game" / "assets", ec) &&
               std::filesystem::is_directory(p / "sim_engine" / "shaders", ec);
    };
    if (is_root(std::filesystem::current_path(ec))) return;

    wchar_t exe_buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();
    for (const auto &cand : {exe_dir, exe_dir.parent_path()}) {
        if (!cand.empty() && is_root(cand)) {
            std::filesystem::current_path(cand, ec);
            return;
        }
    }
#endif
}

} // namespace nsim
