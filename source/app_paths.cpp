#include "app_paths.h"

#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

fs::path resolve_executable_directory() {
#ifdef _WIN32
    // Grow the buffer until the path fits — long paths can exceed MAX_PATH.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (int attempt = 0; attempt < 5; ++attempt) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

        if (written == 0) break;                       // failed outright
        if (written == buffer.size()) {                // truncated — retry bigger
            buffer.resize(buffer.size() * 2);
            continue;
        }

        fs::path exePath(std::wstring(buffer.data(), written));
        fs::path parent = exePath.parent_path();
        if (!parent.empty()) return parent;
        break;
    }
#endif

    // Fallback: behave as before and resolve against the working directory.
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    return ec ? fs::path(".") : cwd;
}

}  // namespace

fs::path app_data_dir() {
    static const fs::path dir = resolve_executable_directory();
    return dir;
}

fs::path app_data_path(const std::string &name) {
    return app_data_dir() / name;
}
