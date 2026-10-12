#include "steam_client_internal.h"

#include "steam_manager.h"
#include "vortex_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#pragma comment(lib, "version.lib")
#endif

using std::string;

namespace {

const char *const kLog = "SteamInstall";

// ── Method positions ────────────────────────────────────────────────────────
// The allow-list. Positions are 0-based vtable slots, from OpenSteamworks'
// cpp/include/steamclient headers and dumped_data/IClientAppManagerMap.json
// (whose "index" comments are 1-based). Every IClientAppManager entry here has
// cannotcallincrossprocess=0 in that map; do not add one that has 1.
enum EngineSlot {
    kCreateSteamPipe      = 0,
    kBReleaseSteamPipe    = 1,
    kConnectToGlobalUser  = 3,
    kReleaseUser          = 6,
    kGetIClientAppManager = 43,
};
enum AppManagerSlot {
    kInstallApp                      = 0,   // (AppId_t, LibraryFolder_t, bool unscheduled) -> EAppError
    kUninstallApp                    = 1,   // (AppId_t) -> EAppError; also cancels a download
    kGetAppInstallState              = 4,   // (AppId_t) -> EAppState
    kGetUpdateInfo                   = 20,  // (AppId_t, AppUpdateInfo_s*) -> bool
    kSetDownloadingEnabled           = 39,  // (bool) -> bool
    kBIsDownloadingEnabled           = 40,  // () -> bool
    kChangeAppDownloadQueuePlacement = 56,  // (AppId_t, EAppDownloadQueuePlacement) -> bool
    kGetNumLibraryFolders            = 75,  // () -> int
    kGetLibraryFolderPath            = 76,  // (LibraryFolder_t, char*, int) -> int
    kGetAppLibraryFolder             = 82,  // (AppId_t) -> LibraryFolder_t
};
constexpr int kPriorityUserInitiated = 2;   // EAppDownloadQueuePlacement
constexpr int kPriorityPaused        = 6;   // what Steam's PauseAppUpdate sets

// AppUpdateInfo_s as the Windows client lays it out. OpenSteamworks' C# struct
// puts the byte counters at +8/+16; on Windows those slots hold the time left
// and an unrelated value, and the counters sit at +64/+72. Read off a 1.27 GB
// install sampled once a second, against the totals content_log.txt printed.
constexpr size_t kUpdateInfoSize       = 512;   // real struct is 244; slack for growth
constexpr size_t kUpdSecondsLeft       = 8;     // u32
constexpr size_t kUpdBytesPerSecond    = 20;    // u32
constexpr size_t kUpdTotal             = 64;    // u64, BytesToStage
constexpr size_t kUpdDone              = 72;    // u64, staged so far
// The network side, found the same way on a 10.0 GB install (2026-10-12): +136
// held content_log's "download 0/10279157904" total exactly, and +128 grew at
// the network rate. +20 above is a disk-write rate; nothing in the struct is a
// network rate, so callers derive one from +128.
constexpr size_t kUpdDownloadDone      = 128;   // u64
constexpr size_t kUpdDownloadTotal     = 136;   // u64

constexpr auto kCallTimeout  = std::chrono::seconds(5);
constexpr auto kIdleRelease  = std::chrono::seconds(30);

template <typename Fn> Fn vslot(void *obj, int slot) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void ***>(obj))[slot]);
}

#ifdef _WIN32
// A wrong slot is an access violation, not an error code. SEH can only guard a
// function with no C++ objects to unwind, so every guarded call goes through
// this one shape: a plain function pointer and an opaque context.
bool seh_guard(void (*fn)(void *), void *ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// MSVC x64 RTTI: vtable[-1] is the CompleteObjectLocator, whose TypeDescriptor
// carries the mangled class name. Read under SEH -- the pointer came from an
// interface we are not yet sure we called correctly.
struct RttiCtx { void *obj; char name[256]; };
void read_rtti(void *p) {
    auto *c = static_cast<RttiCtx *>(p);
    void **vt = *reinterpret_cast<void ***>(c->obj);
    const auto *col = reinterpret_cast<const std::uint32_t *>(vt[-1]);
    if (col[0] != 1) { strcpy_s(c->name, "bad locator"); return; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(col) - col[5];          // pSelf RVA
    const char *name = reinterpret_cast<const char *>(base + col[3] + 16);     // TypeDescriptor.name
    strncpy_s(c->name, name, _TRUNCATE);
}
string rtti_name(void *obj) {
    RttiCtx ctx{obj, {}};
    if (!seh_guard(read_rtti, &ctx)) return "unreadable";
    return ctx.name;
}

string file_version(const fs::path &file) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0) return "unknown";
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO *info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &len) || !info)
        return "unknown";
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
           std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
           std::to_string(LOWORD(info->dwFileVersionLS));
}
#endif

string normalized(const fs::path &p) {
    string s = p.lexically_normal().string();
    std::replace(s.begin(), s.end(), '/', '\\');
    while (s.size() > 3 && s.back() == '\\') s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ── The connection, owned by the worker thread ──────────────────────────────
// Nothing in here is touched from any other thread.
struct Connection {
#ifdef _WIN32
    HMODULE dll = nullptr;
#endif
    void *engine = nullptr;
    int   pipe = 0;
    int   user = 0;
    void *appManager = nullptr;
    bool  validated = false;    // the library check passed once for this DLL load
    bool  disabled = false;     // a check failed: never try again this session
    string reason;
    string loggedReason;        // last failure written to the log
    std::chrono::steady_clock::time_point lastUse;
    std::chrono::steady_clock::time_point lastFailedAttempt;

    bool connected() const { return appManager != nullptr; }
};

#ifdef _WIN32
struct PipeCtx { void *engine; int pipe; int user; void *am; };
void call_create_pipe(void *p) {
    auto *c = static_cast<PipeCtx *>(p);
    c->pipe = vslot<int (*)(void *)>(c->engine, kCreateSteamPipe)(c->engine);
}
void call_connect_user(void *p) {
    auto *c = static_cast<PipeCtx *>(p);
    c->user = vslot<int (*)(void *, int)>(c->engine, kConnectToGlobalUser)(c->engine, c->pipe);
}
void call_get_app_manager(void *p) {
    auto *c = static_cast<PipeCtx *>(p);
    c->am = vslot<void *(*)(void *, int, int)>(c->engine, kGetIClientAppManager)(c->engine, c->user, c->pipe);
}
void call_release(void *p) {
    auto *c = static_cast<PipeCtx *>(p);
    if (c->user)
        vslot<void (*)(void *, int, int)>(c->engine, kReleaseUser)(c->engine, c->pipe, c->user);
    if (c->pipe)
        vslot<bool (*)(void *, int)>(c->engine, kBReleaseSteamPipe)(c->engine, c->pipe);
}

struct FolderCountCtx { void *am; int count; };
void call_folder_count(void *p) {
    auto *c = static_cast<FolderCountCtx *>(p);
    c->count = vslot<int (*)(void *)>(c->am, kGetNumLibraryFolders)(c->am);
}
struct FolderPathCtx { void *am; int index; char path[1024]; int len; };
void call_folder_path(void *p) {
    auto *c = static_cast<FolderPathCtx *>(p);
    c->len = vslot<int (*)(void *, int, char *, int)>(c->am, kGetLibraryFolderPath)(
        c->am, c->index, c->path, static_cast<int>(sizeof c->path));
}

struct InstallCtx {
    void *am; std::uint32_t appid; int folder;
    std::uint32_t error; bool downloadsWereEnabled; bool placed; bool enabledNow;
};
void call_install(void *p) {
    auto *c = static_cast<InstallCtx *>(p);
    c->error = vslot<std::uint32_t (*)(void *, std::uint32_t, int, bool)>(c->am, kInstallApp)(
        c->am, c->appid, c->folder, true);
}
struct UninstallCtx { void *am; std::uint32_t appid; std::uint32_t error; };
void call_uninstall(void *p) {
    auto *c = static_cast<UninstallCtx *>(p);
    c->error = vslot<std::uint32_t (*)(void *, std::uint32_t)>(c->am, kUninstallApp)(c->am, c->appid);
}

void call_queue_front(void *p) {
    auto *c = static_cast<InstallCtx *>(p);
    c->downloadsWereEnabled = vslot<bool (*)(void *)>(c->am, kBIsDownloadingEnabled)(c->am);
    c->placed = vslot<bool (*)(void *, std::uint32_t, int)>(c->am, kChangeAppDownloadQueuePlacement)(
        c->am, c->appid, kPriorityUserInitiated);
    c->enabledNow = c->downloadsWereEnabled;
    if (!c->downloadsWereEnabled)
        c->enabledNow = vslot<bool (*)(void *, bool)>(c->am, kSetDownloadingEnabled)(c->am, true);
}

// Steam's PauseAppUpdate: the app leaves the download queue and keeps what it
// has fetched. Only this app -- SetDownloadingEnabled(false) would stop every
// download Steam has.
struct PauseCtx { void *am; std::uint32_t appid; bool placed; };
void call_pause(void *p) {
    auto *c = static_cast<PauseCtx *>(p);
    c->placed = vslot<bool (*)(void *, std::uint32_t, int)>(c->am, kChangeAppDownloadQueuePlacement)(
        c->am, c->appid, kPriorityPaused);
}

struct ProgressCtx {
    void *am; std::uint32_t appid;
    std::uint32_t state; int folder; bool infoOk;
    alignas(8) unsigned char info[kUpdateInfoSize];
};
void call_progress(void *p) {
    auto *c = static_cast<ProgressCtx *>(p);
    c->state  = vslot<std::uint32_t (*)(void *, std::uint32_t)>(c->am, kGetAppInstallState)(c->am, c->appid);
    c->folder = vslot<int (*)(void *, std::uint32_t)>(c->am, kGetAppLibraryFolder)(c->am, c->appid);
    c->infoOk = vslot<bool (*)(void *, std::uint32_t, void *)>(c->am, kGetUpdateInfo)(c->am, c->appid, c->info);
}
#endif

class Worker {
public:
    static Worker &instance() {
        static Worker worker;
        return worker;
    }

    // Runs fn on the worker thread and waits for it. nullopt on timeout, or
    // once a timeout has happened: the worker is then presumed stuck inside a
    // call to Steam, and queueing behind it would only stall the next caller.
    template <typename R> std::optional<R> run(std::function<R(Connection &)> fn) {
        if (m_wedged.load()) return std::nullopt;
        auto task = std::make_shared<std::packaged_task<R()>>([this, fn]() { return fn(m_conn); });
        std::future<R> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) return std::nullopt;
            ensure_thread_locked();
            m_queue.emplace_back([task]() { (*task)(); });
        }
        m_wake.notify_one();
        if (result.wait_for(kCallTimeout) != std::future_status::ready) {
            m_wedged = true;
            vlog::item(kLog, "Steam client call", vlog::Status::Fail,
                       "no answer within 5s -- internal installs off for this session");
            return std::nullopt;
        }
        return result.get();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_thread.joinable() || m_stopping) return;
            m_stopping = true;
        }
        m_wake.notify_one();
        if (m_wedged.load()) m_thread.detach();
        else m_thread.join();
    }

    ~Worker() { shutdown(); }

private:
    Worker() = default;

    void ensure_thread_locked() {
        if (!m_thread.joinable())
            m_thread = std::thread([this]() { loop(); });
    }

    void loop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait_for(lock, std::chrono::seconds(1),
                                [this]() { return m_stopping || !m_queue.empty(); });
                if (!m_queue.empty()) {
                    job = std::move(m_queue.front());
                    m_queue.pop_front();
                } else if (m_stopping) {
                    break;
                }
            }
            if (job) {
                job();
                m_conn.lastUse = std::chrono::steady_clock::now();
            } else if (m_conn.connected() &&
                       std::chrono::steady_clock::now() - m_conn.lastUse > kIdleRelease) {
                release(m_conn, "idle");
            }
        }
        if (m_conn.connected()) release(m_conn, "launcher exiting");
    }

public:
    static void release(Connection &c, const char *why) {
#ifdef _WIN32
        PipeCtx ctx{c.engine, c.pipe, c.user, nullptr};
        if (c.engine) seh_guard(call_release, &ctx);
#endif
        c.pipe = 0;
        c.user = 0;
        c.appManager = nullptr;
        vlog::line(kLog, string("Released Steam client pipe (") + why + ")");
    }

private:
    Connection              m_conn;
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_queue;
    bool                    m_stopping = false;
    std::atomic<bool>       m_wedged{false};
};

// Worker thread only. Loads the DLL once, opens a pipe, and on the first
// connection checks that the slots above still mean what they meant: the app
// manager's RTTI must name an IClientAppManager, and Steam's library list must
// be the one libraryfolders.vdf holds. A slot that has moved fails one of the
// two long before InstallApp is reached.
bool connect_locked(Connection &c) {
    if (c.connected()) return true;
    if (c.disabled) return false;
    // Steam closed: the progress poll asks every second, and each attempt
    // would otherwise reload nothing but still knock on a pipe that is not
    // there.
    const auto now = std::chrono::steady_clock::now();
    if (!c.reason.empty() && now - c.lastFailedAttempt < std::chrono::seconds(10)) return false;
#ifdef _WIN32
    const auto fail = [&c, now](const string &step, const string &reason, bool permanent) {
        c.reason = reason;
        c.lastFailedAttempt = now;
        if (permanent) c.disabled = true;
        if (reason != c.loggedReason) {
            vlog::item(kLog, step, permanent ? vlog::Status::Fail : vlog::Status::Skipped, reason);
            c.loggedReason = reason;
        }
        return false;
    };

    if (!c.dll) {
        const fs::path steamPath = steam_install_path();
        if (steamPath.empty())
            return fail("Steam client", "Steam is not installed (no SteamPath in the registry)", false);
        const fs::path dll = steamPath / "steamclient64.dll";
        std::error_code ec;
        const auto size = fs::file_size(dll, ec);
        vlog::line(kLog, "Steam path: " + steamPath.string());
        vlog::line(kLog, "steamclient64.dll: " + dll.string() + ", version " + file_version(dll) +
                             ", " + (ec ? string("size unknown") : std::to_string(size) + " bytes"));

        // LOAD_WITH_ALTERED_SEARCH_PATH: its own dependencies (tier0_s64,
        // vstdlib_s64) are found beside it rather than beside the launcher.
        c.dll = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!c.dll)
            return fail("LoadLibraryEx", "could not load steamclient64.dll (error " +
                                             std::to_string(GetLastError()) + ")", true);
        vlog::item(kLog, "LoadLibraryEx steamclient64.dll", vlog::Status::Ok);

        using CreateInterfaceFn = void *(*)(const char *, int *);
        auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(c.dll, "CreateInterface"));
        if (!create)
            return fail("CreateInterface", "steamclient64.dll has no CreateInterface export", true);
        c.engine = create("CLIENTENGINE_INTERFACE_VERSION005", nullptr);
        if (!c.engine)
            return fail("CreateInterface", "CLIENTENGINE_INTERFACE_VERSION005 not offered", true);
        vlog::item(kLog, "CreateInterface CLIENTENGINE_INTERFACE_VERSION005", vlog::Status::Ok,
                   "RTTI " + rtti_name(c.engine));
    }

    PipeCtx ctx{c.engine, 0, 0, nullptr};
    if (!seh_guard(call_create_pipe, &ctx))
        return fail("CreateSteamPipe", "crashed calling slot " + std::to_string(kCreateSteamPipe), true);
    if (!ctx.pipe)
        return fail("CreateSteamPipe", "no pipe -- Steam is not running", false);
    if (!seh_guard(call_connect_user, &ctx))
        return fail("ConnectToGlobalUser", "crashed calling slot " + std::to_string(kConnectToGlobalUser), true);
    if (!ctx.user) {
        seh_guard(call_release, &ctx);
        return fail("ConnectToGlobalUser", "no user -- Steam is not signed in", false);
    }
    vlog::item(kLog, "Steam pipe and user", vlog::Status::Ok, "pipe present, user present");

    if (!seh_guard(call_get_app_manager, &ctx) || !ctx.am) {
        seh_guard(call_release, &ctx);
        return fail("GetIClientAppManager", "slot " + std::to_string(kGetIClientAppManager) +
                                                " returned nothing usable", true);
    }
    const string rtti = rtti_name(ctx.am);
    if (rtti.find("IClientAppManager") == string::npos) {
        seh_guard(call_release, &ctx);
        return fail("GetIClientAppManager", "object is " + rtti + ", not an IClientAppManager -- "
                                                "Steam's interface layout has changed", true);
    }

    c.pipe = ctx.pipe;
    c.user = ctx.user;
    c.appManager = ctx.am;
    c.lastUse = std::chrono::steady_clock::now();

    if (!c.validated) {
        vlog::line(kLog, "IClientAppManager RTTI " + rtti + "; slots InstallApp=" +
                             std::to_string(kInstallApp) + " GetAppInstallState=" +
                             std::to_string(kGetAppInstallState) + " GetUpdateInfo=" +
                             std::to_string(kGetUpdateInfo) + " ChangeAppDownloadQueuePlacement=" +
                             std::to_string(kChangeAppDownloadQueuePlacement) + " GetNumLibraryFolders=" +
                             std::to_string(kGetNumLibraryFolders) + " GetLibraryFolderPath=" +
                             std::to_string(kGetLibraryFolderPath) + " GetAppLibraryFolder=" +
                             std::to_string(kGetAppLibraryFolder));

        FolderCountCtx count{c.appManager, 0};
        if (!seh_guard(call_folder_count, &count) || count.count < 1 || count.count > 64) {
            Worker::release(c, "validation failed");
            return fail("GetNumLibraryFolders", "answered " + std::to_string(count.count) +
                                                    " -- slot layout does not match", true);
        }
        std::vector<string> known;
        for (const fs::path &lib : steam_library_folders()) known.push_back(normalized(lib));
        for (int i = 0; i < count.count; ++i) {
            FolderPathCtx path{c.appManager, i, {}, 0};
            const bool ok = seh_guard(call_folder_path, &path) && path.len > 0;
            const bool matches = ok && std::find(known.begin(), known.end(),
                                                 normalized(path.path)) != known.end();
            vlog::item(kLog, "library folder " + std::to_string(i), matches ? vlog::Status::Ok : vlog::Status::Fail,
                       string(ok ? path.path : "(no path)") +
                           (matches ? " -- matches libraryfolders.vdf" : " -- NOT in libraryfolders.vdf"));
            if (!matches) {
                Worker::release(c, "validation failed");
                return fail("GetLibraryFolderPath", "Steam's library list does not match libraryfolders.vdf -- "
                                                    "slot layout has changed", true);
            }
        }
        c.validated = true;
    }
    vlog::item(kLog, "Steam client route", vlog::Status::Ok, "available");
    c.reason.clear();
    c.loggedReason.clear();
    return true;
#else
    c.disabled = true;
    c.reason = "only available on Windows";
    return false;
#endif
}

}  // namespace

bool steam_internal_available() {
    auto r = Worker::instance().run<bool>([](Connection &c) { return connect_locked(c); });
    return r.value_or(false);
}

std::string steam_internal_unavailable_reason() {
    auto r = Worker::instance().run<string>([](Connection &c) { return c.reason; });
    return r.value_or("the Steam client stopped answering");
}

std::vector<SteamInternalFolder> steam_internal_library_folders() {
    using Folders = std::vector<SteamInternalFolder>;
    auto r = Worker::instance().run<Folders>([](Connection &c) {
        Folders out;
        if (!connect_locked(c)) return out;
#ifdef _WIN32
        FolderCountCtx count{c.appManager, 0};
        if (!seh_guard(call_folder_count, &count)) return out;
        for (int i = 0; i < count.count && i < 64; ++i) {
            FolderPathCtx path{c.appManager, i, {}, 0};
            if (seh_guard(call_folder_path, &path) && path.len > 0)
                out.push_back({i, fs::path(path.path)});
        }
#endif
        return out;
    });
    return r.value_or(Folders{});
}

int steam_internal_install(int appid, int folderIndex) {
    auto r = Worker::instance().run<int>([appid, folderIndex](Connection &c) -> int {
        if (!connect_locked(c)) return -1;
#ifdef _WIN32
        InstallCtx ctx{c.appManager, static_cast<std::uint32_t>(appid), folderIndex, 0, false, false, false};
        vlog::line(kLog, "InstallApp(" + std::to_string(appid) + ", folder " +
                             std::to_string(folderIndex) + ", unscheduled=true)");
        if (!seh_guard(call_install, &ctx)) {
            c.disabled = true;
            c.reason = "InstallApp crashed";
            Worker::release(c, "InstallApp crashed");
            vlog::item(kLog, "InstallApp", vlog::Status::Fail, "access violation -- route disabled");
            return -1;
        }
        const int err = static_cast<int>(ctx.error);
        vlog::item(kLog, "InstallApp", err == 0 ? vlog::Status::Ok : vlog::Status::Fail,
                   "EAppError " + std::to_string(err) + " (" + steam_app_error_name(err) + ")");
        if (err != 0) return err;

        // InstallApp leaves the app queued and paused; this is what Steam's
        // own Downloads page does to start one.
        if (!seh_guard(call_queue_front, &ctx)) {
            vlog::item(kLog, "ChangeAppDownloadQueuePlacement", vlog::Status::Fail, "access violation");
            return 0;   // installed into the queue all the same
        }
        vlog::item(kLog, "ChangeAppDownloadQueuePlacement(PriorityUserInitiated)",
                   ctx.placed ? vlog::Status::Ok : vlog::Status::Fail);
        if (!ctx.downloadsWereEnabled)
            vlog::item(kLog, "SetDownloadingEnabled(true)", ctx.enabledNow ? vlog::Status::Ok : vlog::Status::Fail,
                       "downloads were switched off in Steam");
        return 0;
#else
        return -1;
#endif
    });
    return r.value_or(-1);
}

bool steam_internal_set_paused(int appid, bool paused) {
    auto r = Worker::instance().run<bool>([appid, paused](Connection &c) -> bool {
        if (!connect_locked(c)) return false;
#ifdef _WIN32
        const char *what = paused ? "PauseAppUpdate" : "ResumeAppUpdate";
        bool ok = false;
        bool guarded = false;
        if (paused) {
            PauseCtx ctx{c.appManager, static_cast<std::uint32_t>(appid), false};
            guarded = seh_guard(call_pause, &ctx);
            ok = ctx.placed;
        } else {
            InstallCtx ctx{c.appManager, static_cast<std::uint32_t>(appid), -1, 0, false, false, false};
            guarded = seh_guard(call_queue_front, &ctx);
            ok = ctx.placed;
            if (guarded && !ctx.downloadsWereEnabled)
                vlog::item(kLog, "SetDownloadingEnabled(true)", ctx.enabledNow ? vlog::Status::Ok : vlog::Status::Fail,
                           "downloads were switched off in Steam");
        }
        if (!guarded) {
            c.disabled = true;
            c.reason = string(what) + " crashed";
            Worker::release(c, c.reason.c_str());
            vlog::item(kLog, what, vlog::Status::Fail, "access violation -- route disabled");
            return false;
        }
        vlog::item(kLog, string(what) + "(" + std::to_string(appid) + ")",
                   ok ? vlog::Status::Ok : vlog::Status::Fail,
                   string("ChangeAppDownloadQueuePlacement ") + (paused ? "PriorityPaused" : "PriorityUserInitiated"));
        return ok;
#else
        return false;
#endif
    });
    return r.value_or(false);
}

int steam_internal_uninstall(int appid) {
    auto r = Worker::instance().run<int>([appid](Connection &c) -> int {
        if (!connect_locked(c)) return -1;
#ifdef _WIN32
        UninstallCtx ctx{c.appManager, static_cast<std::uint32_t>(appid), 0};
        vlog::line(kLog, "UninstallApp(" + std::to_string(appid) + ")");
        if (!seh_guard(call_uninstall, &ctx)) {
            c.disabled = true;
            c.reason = "UninstallApp crashed";
            Worker::release(c, "UninstallApp crashed");
            vlog::item(kLog, "UninstallApp", vlog::Status::Fail, "access violation -- route disabled");
            return -1;
        }
        const int err = static_cast<int>(ctx.error);
        vlog::item(kLog, "UninstallApp", err == 0 ? vlog::Status::Ok : vlog::Status::Fail,
                   "EAppError " + std::to_string(err) + " (" + steam_app_error_name(err) + ")");
        return err;
#else
        return -1;
#endif
    });
    return r.value_or(-1);
}

SteamInternalProgress steam_internal_progress(int appid) {
    auto r = Worker::instance().run<SteamInternalProgress>([appid](Connection &c) {
        SteamInternalProgress out;
        if (!connect_locked(c)) return out;
#ifdef _WIN32
        auto ctx = std::make_unique<ProgressCtx>();
        ctx->am = c.appManager;
        ctx->appid = static_cast<std::uint32_t>(appid);
        if (!seh_guard(call_progress, ctx.get())) {
            c.disabled = true;
            c.reason = "progress call crashed";
            Worker::release(c, "progress call crashed");
            vlog::item(kLog, "GetUpdateInfo", vlog::Status::Fail, "access violation -- route disabled");
            return out;
        }
        // EAppState 0 is "invalid": the pipe is open but nothing answers on
        // it, which is what a pipe to a Steam that has since exited looks
        // like. Reconnect on the next call rather than report it.
        if (ctx->state == 0) {
            Worker::release(c, "Steam stopped answering");
            return out;
        }
        out.ok = true;
        out.state = ctx->state;
        out.folder = ctx->folder;
        if (ctx->infoOk) {
            std::memcpy(&out.secondsLeft, ctx->info + kUpdSecondsLeft, 4);
            std::memcpy(&out.bytesPerSecond, ctx->info + kUpdBytesPerSecond, 4);
            std::memcpy(&out.total, ctx->info + kUpdTotal, 8);
            std::memcpy(&out.done, ctx->info + kUpdDone, 8);
            // A layout change shows up as nonsense here first.
            out.haveBytes = out.total > 0 && out.done <= out.total &&
                            out.total < (1ull << 42);   // 4 TB
            std::memcpy(&out.downloadDone, ctx->info + kUpdDownloadDone, 8);
            std::memcpy(&out.downloadTotal, ctx->info + kUpdDownloadTotal, 8);
            out.haveDownload = out.downloadTotal > 0 && out.downloadDone <= out.downloadTotal &&
                               out.downloadTotal < (1ull << 42);
        }
#endif
        return out;
    });
    return r.value_or(SteamInternalProgress{});
}

const char *steam_app_error_name(int error) {
    static const char *const kNames[] = {
        "NoError", "UnspecifiedError", "Paused", "Canceled", "Suspended", "NoSubscription",
        "NoConnection", "ConnectionTimeout", "MissingDecryptionKey", "MissingConfiguration",
        "DiskReadFailure", "DiskWriteFailure", "NotEnoughDiskSpace", "CorruptGameFiles",
        "WaitingForDisk", "InvalidInstallPath", "ApplicationRunning", "DependencyFailure",
        "NotInstalled", "UpdateRequired", "StillBusy", "NoConnectionToContentServers",
        "InvalidApplicationConfiguration", "InvalidContentConfiguration", "ManifestUnavailable",
        "NotReleased", "RegionRestricted", "CorruptDepotCache", "MissingExecutable",
        "InvalidPlatform", "InvalidFileSystem", "CorruptUpdateFiles", "DownloadDisabled",
        "SharedLibraryLocked", "PurchasePending", "OtherSessionPlaying", "CorruptDownload",
        "CorruptDisk", "MissingFilePermissions", "FileLocked", "ContentUnavailable",
        "Requires64bitOperatingSystem", "MissingUpdateFiles", "NotEnoughDiskQuota",
        "SiteLicenseLocked", "ParentalControlBlocked", "CreateProcessFailed",
        "SteamClientOutOfDate", "AllowedPlaytimeExceeded", "CorruptFileSignature",
        "MissingGameFiles", "CompatToolFailed", "InstallPathRemoved", "InvalidBackupPath",
        "InvalidPasscode", "SelfUpdating", "AllowedPlaytimeExceeded2",
    };
    if (error == -1) return "route unavailable";
    if (error < 0 || error >= static_cast<int>(sizeof kNames / sizeof kNames[0])) return "unknown";
    return kNames[error];
}

void steam_internal_shutdown() {
    Worker::instance().shutdown();
}
