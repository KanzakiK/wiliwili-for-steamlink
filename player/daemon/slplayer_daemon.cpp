/*
 * slplayer_daemon - 播放子进程（阶段 3.5a 进程隔离）
 *
 * SlPlayer 跑在本进程，wiliwili 主进程内的 libmpv 兼容层通过 Unix socket
 * 控制播放。控制面/事件面走 IPC，数据面（视频/音频）由 SLVideo/SLAudio
 * 走硬件层，不经过 IPC。
 *
 * 协议见 player/include/IpcProtocol.h。
 *
 * 生命周期：
 *   - 启动后 bind + listen，等待兼容层 connect（单客户端）。
 *   - 收到 open 命令才真正 open SlPlayer；quit 命令优雅退出（走 SlPlayer::stop
 *     → Dirty Flag 清除）。父进程死亡由兼容层侧的 PDEATHSIG/EOF 触发本进程退出
 *     （见 06 文档孤儿清理设计）。
 */
#include "SlPlayer.h"
#include "IpcProtocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_listenFd = -1;
int g_clientFd = -1;
std::mutex g_writeMutex;
std::atomic<bool> g_quit{false};
/* 当前文件代次。P0：所有事件都带它，兼容层可丢弃旧文件的迟到事件。 */
std::atomic<std::uint64_t> g_fileGeneration{0};
/* D27：按 generation 保存 debug session。meta 先于 open 到达；
 * watchdog 回放同一 open 时复用已保存的 meta。 */
std::mutex g_sessionMetaMutex;
std::map<std::uint64_t, sl_session_meta_t> g_sessionMeta;
/* D28：只有一个 pending sync context，按 (gen, req) 一次性消费。 */
std::mutex g_syncContextMutex;
sl_sync_context_t g_pendingSync = {};

static const char* dash(const char* value) { return value && *value ? value : "-"; }

static sl::DebugLogContext debugContextFromMeta(std::uint64_t gen,
                                                const sl_session_meta_t& meta,
                                                std::uint64_t req = 0) {
    sl::DebugLogContext context;
    context.run = meta.run;
    context.session = meta.session;
    context.content = meta.content;
    context.id = meta.id;
    context.bv = meta.bv;
    context.cid = meta.cid;
    context.qn = meta.qn;
    context.generation = gen;
    context.requestId = req;
    return context;
}

static std::string metaIdentity(const sl_session_meta_t& meta) {
    return std::string("content=") + dash(meta.content) +
           " id=" + dash(meta.id) + " bv=" + dash(meta.bv) +
           " cid=" + dash(meta.cid) + " qn=" + dash(meta.qn);
}

static sl::SyncContext syncContextFromMeta(const sl_sync_meta_t& meta,
                                           std::uint64_t requestId) {
    sl::SyncContext context;
    context.epoch = meta.epoch;
    context.targetMs = meta.target_ms;
    context.reason = meta.reason;
    context.desiredPlay = std::strcmp(meta.desired, SL_SYNC_DESIRED_PAUSE) != 0;
    context.requestId = requestId;
    return context;
}

static void daemonLogV(const char* source, std::uint64_t req,
                       const char* fmt, va_list args) {
    char message[2048];
    vsnprintf(message, sizeof(message), fmt, args);
    sl::d27LogReq(source, req, "%s", message);
}

static void daemonLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    daemonLogV("DAEMON", 0, fmt, args);
    va_end(args);
}

static void daemonLogReq(std::uint64_t req, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    daemonLogV("DAEMON", req, fmt, args);
    va_end(args);
}

static void daemonLogContext(const sl::DebugLogContext& context,
                             const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    sl::d27LogContext("DAEMON", context, "%s", message);
}

static bool consumeSyncContext(std::uint64_t gen, std::uint64_t req,
                               const sl::DebugLogContext& context,
                               sl_sync_meta_t* out) {
    sl_sync_context_t snapshot = {};
    sl_sync_meta_t consumed = {};
    int mismatch = 0;
    int ok = 0;
    {
        std::lock_guard<std::mutex> lock(g_syncContextMutex);
        snapshot = g_pendingSync;
        ok = sl_sync_context_consume(&g_pendingSync, gen, req, &consumed,
                                     &mismatch);
    }
    if (ok) {
        if (out) *out = consumed;
        daemonLogContext(context,
                         "sync meta consumed epoch=%llu target_ms=%lld "
                         "reason=%s desired=%s",
                         (unsigned long long)consumed.epoch,
                         (long long)consumed.target_ms, consumed.reason,
                         consumed.desired);
        return true;
    }
    if (mismatch && snapshot.valid) {
        daemonLogContext(context,
                         "sync meta discarded reason=identity_mismatch "
                         "pending_gen=%llu pending_req=%llu "
                         "command_gen=%llu command_req=%llu",
                         (unsigned long long)snapshot.generation,
                         (unsigned long long)snapshot.requestId,
                         (unsigned long long)gen, (unsigned long long)req);
    }
    return false;
}

static void storeSessionMeta(std::uint64_t gen, const sl_session_meta_t& meta) {
    std::lock_guard<std::mutex> lock(g_sessionMetaMutex);
    g_sessionMeta[gen] = meta;
    while (g_sessionMeta.size() > 8)
        g_sessionMeta.erase(g_sessionMeta.begin());
}

static bool getSessionMeta(std::uint64_t gen, sl_session_meta_t& meta) {
    std::lock_guard<std::mutex> lock(g_sessionMetaMutex);
    auto it = g_sessionMeta.find(gen);
    if (it == g_sessionMeta.end()) return false;
    meta = it->second;
    return true;
}

/* PR_SET_PDEATHSIG 是「父线程死亡」语义；handler 只置 sig_atomic_t 标志。
 * 真正是否退出由主循环复核 getppid()，避免创建 daemon 的线程先退出导致误杀。 */
volatile sig_atomic_t g_parentDeathSignal = 0;
pid_t g_expectedParentPid = 0;

const char* socketPath() {
    const char* env = getenv(SL_IPC_ENV_SOCKET);
    return env && *env ? env : SL_IPC_DEFAULT_SOCKET;
}

/* ---- 帧读写（4 字节小端长度 + payload）---- */
bool sendPayload(int fd, const std::string& payload) {
    if (fd < 0 || payload.size() > SL_IPC_MAX_PAYLOAD) return false;
    uint32_t len = static_cast<uint32_t>(payload.size());
    unsigned char header[SL_IPC_HEADER_SIZE] = {
        static_cast<unsigned char>(len & 0xff),
        static_cast<unsigned char>((len >> 8) & 0xff),
        static_cast<unsigned char>((len >> 16) & 0xff),
        static_cast<unsigned char>((len >> 24) & 0xff),
    };
    std::lock_guard<std::mutex> lock(g_writeMutex);
    size_t off = 0;
    while (off < SL_IPC_HEADER_SIZE) {
        ssize_t n = write(fd, header + off, SL_IPC_HEADER_SIZE - off);
        if (n > 0) off += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    off = 0;
    while (off < payload.size()) {
        ssize_t n = write(fd, payload.data() + off, payload.size() - off);
        if (n > 0) off += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

/* 返回 1 = 收到完整帧；0 = 超时无数据（主循环继续，检查 g_quit）；-1 = 断开 */
int recvPayload(int fd, std::string& out) {
    unsigned char header[SL_IPC_HEADER_SIZE];
    size_t off = 0;
    while (off < SL_IPC_HEADER_SIZE) {
        ssize_t n = read(fd, header + off, SL_IPC_HEADER_SIZE - off);
        if (n > 0) off += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return off == 0 ? 0 : -1; /* 帧中途超时 = 对端发了半帧后卡住，视为断开 */
        else return -1;  /* EOF 或其它错误：对端断开 */
    }
    uint32_t len = static_cast<uint32_t>(header[0]) |
                   (static_cast<uint32_t>(header[1]) << 8) |
                   (static_cast<uint32_t>(header[2]) << 16) |
                   (static_cast<uint32_t>(header[3]) << 24);
    if (len > SL_IPC_MAX_PAYLOAD) return -1;
    out.assign(len, '\0');
    off = 0;
    while (off < len) {
        ssize_t n = read(fd, &out[off], len - off);
        if (n > 0) off += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
        else return -1;
    }
    return 1;
}

/* ---- payload 字段切分（TAB）---- */
std::vector<std::string> splitFields(const std::string& s) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(SL_IPC_FIELD_SEP, start);
        if (pos == std::string::npos) {
            fields.emplace_back(s.substr(start));
            break;
        }
        fields.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

void emitContext(const char* event, std::uint64_t requestId,
                 const std::string& args = std::string()) {
    char prefix[80];
    snprintf(prefix, sizeof(prefix), "%llu\t%llu",
             (unsigned long long)g_fileGeneration.load(),
             (unsigned long long)requestId);
    std::string payload = event;
    payload += SL_IPC_FIELD_SEP;
    payload += prefix;
    if (!args.empty()) {
        payload += SL_IPC_FIELD_SEP;
        payload += args;
    }
    if (!sendPayload(g_clientFd, payload)) {
        /* 写失败通常意味着父进程断开；交给主循环 EOF 处理 */
    }
}

void emit(const char* event, const std::string& args = std::string()) {
    emitContext(event, 0, args);
}

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool checkParentDeath() {
    if (!g_parentDeathSignal) return false;
    g_parentDeathSignal = 0;
    if (g_expectedParentPid > 0 && getppid() == g_expectedParentPid) {
        daemonLog("忽略 PDEATHSIG：父线程退出但父进程仍在(pid=%d)",
                  (int)g_expectedParentPid);
        return false;
    }
    daemonLog("父进程已退出(expected=%d current=%d)，优雅退出",
              (int)g_expectedParentPid, (int)getppid());
    g_quit.store(true);
    return true;
}

std::uint64_t parseUnsigned(const std::string& s) {
    return strtoull(s.c_str(), nullptr, 10);
}

/* 旧文件命令不得影响当前播放代次。open 自己负责建立新代次，不走这里。 */
bool commandGenerationMatches(const std::vector<std::string>& f) {
    if (f.size() < 3) {
        daemonLog("命令缺少 P0 帧信封: %s", f[0].c_str());
        return false;
    }
    std::uint64_t gen = parseUnsigned(f[1]);
    if (gen != g_fileGeneration.load()) {
        daemonLog("丢弃过期代次命令: cmd=%s gen=%llu current=%llu",
                  f[0].c_str(), (unsigned long long)gen,
                  (unsigned long long)g_fileGeneration.load());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    g_expectedParentPid = getppid();
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--expected-parent") == 0) {
            pid_t expected = static_cast<pid_t>(atoi(argv[i + 1]));
            if (expected > 0) g_expectedParentPid = expected;
            ++i;
        }
    }

    /* 父进程意外退出时，PDEATHSIG 由兼容层在 fork 后的子进程里设置。
     * handler 只置标志；是否退出由主循环复核 getppid() 后决定。 */
    signal(SIGTERM, [](int) { g_parentDeathSignal = 1; });
    signal(SIGPIPE, SIG_IGN); /* 写断开的 socket 不要杀死进程 */

    /* 兼容层在 fork 后、exec 前阻塞了 SIGTERM；handler 装好后才解除，
     * 避免父线程在这段窗口内退出时按默认动作杀死子进程。 */
    sigset_t termSet;
    sigemptyset(&termSet);
    sigaddset(&termSet, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &termSet, NULL);

    const char* path = socketPath();
    unlink(path); /* 清理上次残留 */

    g_listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listenFd < 0) {
        daemonLog("socket 失败: %s", strerror(errno));
        return 1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        daemonLog("socket 路径过长");
        return 1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(g_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        daemonLog("bind 失败: %s", strerror(errno));
        return 1;
    }
    if (listen(g_listenFd, 1) < 0) {
        daemonLog("listen 失败: %s", strerror(errno));
        return 1;
    }
    daemonLog("listening on %s", path);

    /* 等待兼容层连接。带超时，避免父进程启动失败时子进程永久挂着占 VPU。 */
    /* 15s 总超时；200ms 轮询一次父进程死亡信号，满足「父进程死后 <1s 退出」。 */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000;
    setsockopt(g_listenFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const std::int64_t acceptDeadline = steadyNowMs() + 15000;
    while (!g_quit.load() && steadyNowMs() < acceptDeadline) {
        checkParentDeath();
        if (g_quit.load()) break;
        g_clientFd = accept(g_listenFd, nullptr, nullptr);
        if (g_clientFd >= 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
        g_clientFd = -1;
    }
    if (g_clientFd < 0) {
        daemonLog("accept 失败/超时/父进程退出: %s", strerror(errno));
        close(g_listenFd);
        unlink(path);
        return 0;
    }
    daemonLog("client connected");

    /* ---- SlPlayer 实例 ---- */
    sl::Player player;
    bool opened = false;
    std::mutex playerMutex; /* 保护 open/stop 与命令处理串行 */
    auto lastProgressMs = std::atomic<std::int64_t>{0};

    player.setEventCallback([&](const sl::Event& ev) {
        switch (ev.type) {
        case sl::EventType::Opened: {
            sl::VideoInfo info = player.videoInfo();
            char args[128];
            snprintf(args, sizeof(args), "%.3f\t%d\t%d", ev.duration,
                     info.width, info.height);
            emitContext(SL_EVT_OPENED, ev.requestId, args);
            sl::d27LogReq("EVT", ev.requestId,
                          "opened duration=%.3f width=%d height=%d status=ok",
                          ev.duration, info.width, info.height);
            break;
        }
        case sl::EventType::PlaybackStart:
            emitContext(SL_EVT_PLAYBACK_START, ev.requestId);
            sl::d27LogReq("EVT", ev.requestId, "playback start status=ok");
            break;
        case sl::EventType::Progress: {
            std::int64_t now = steadyNowMs();
            if (now - lastProgressMs.load() >= SL_EVT_PROGRESS_INTERVAL_MS) {
                lastProgressMs.store(now);
                char args[128];
                snprintf(args, sizeof(args), "%.3f\t%.3f", ev.position, ev.duration);
                emitContext(SL_EVT_PROGRESS, ev.requestId, args);
            }
            break;
        }
        case sl::EventType::Paused:
            emitContext(SL_EVT_PAUSED, ev.requestId);
            sl::d27LogReq("EVT", ev.requestId, "paused status=ok");
            break;
        case sl::EventType::Resumed:
            emitContext(SL_EVT_RESUMED, ev.requestId);
            sl::d27LogReq("EVT", ev.requestId, "resumed status=ok");
            break;
        case sl::EventType::Seeked: {
            char args[64];
            snprintf(args, sizeof(args), "%.3f", ev.position);
            emitContext(SL_EVT_SEEKED, ev.requestId, args);
            sl::d27LogReq("EVT", ev.requestId,
                          "seeked position=%.3f status=ok", ev.position);
            break;
        }
        case sl::EventType::EndFile:
            emitContext(SL_EVT_EOF, ev.requestId);
            sl::d27LogReq("EVT", ev.requestId, "eof status=ok");
            break;
        case sl::EventType::Error:
            emitContext(SL_EVT_ERROR, ev.requestId, ev.message);
            sl::d27LogReq("EVT", ev.requestId, "error message=%s", ev.message.c_str());
            break;
        case sl::EventType::Unsupported:
            emitContext(SL_EVT_ERROR, ev.requestId, "unsupported stream");
            sl::d27LogReq("EVT", ev.requestId,
                          "error message=unsupported stream");
            break;
        }
    });
    player.setFatalCallback([](sl::FatalType type, const std::string& message) {
        char args[512];
        snprintf(args, sizeof(args), "%s\t%s", sl::fatalTypeName(type), message.c_str());
        emit(SL_EVT_FATAL, args);
        sl::d27LogReq("EVT", 0, "fatal type=%s message=%s",
                      sl::fatalTypeName(type), message.c_str());
    });

    /* recv 超时，让主循环能定期检查 g_quit（PDEATHSIG/SIGTERM） */
    tv.tv_sec = 0;
    tv.tv_usec = 200 * 1000;
    setsockopt(g_clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string payload;
    while (!g_quit.load()) {
        checkParentDeath();
        if (g_quit.load()) break;
        int rr = recvPayload(g_clientFd, payload);
        if (rr < 0) {
            daemonLogReq(0, "control disconnect opened=%d gen=%llu",
                         opened ? 1 : 0,
                         (unsigned long long)g_fileGeneration.load());
            daemonLog("控制连接断开，退出");
            break;
        }
        if (rr == 0) continue; /* 超时：回到循环头检查 g_quit */
        std::vector<std::string> f = splitFields(payload);
        if (f.empty()) continue;
        const std::string& cmd = f[0];

        if (cmd == SL_CMD_META) {
            /* meta<TAB>gen<TAB>req<TAB>run=...<TAB>session=...<TAB>content=...
             * <TAB>id=...<TAB>bv=...<TAB>cid=...<TAB>qn=...
             * 可追加 epoch/target_ms/reason/desired；两类 meta 独立解析。 */
            if (f.size() < 5) {
                daemonLog("bad meta command");
                continue;
            }
            std::uint64_t gen = parseUnsigned(f[1]);
            std::uint64_t req = parseUnsigned(f[2]);
            std::string kv;
            for (size_t i = 3; i < f.size(); ++i) {
                if (i > 3) kv += SL_IPC_FIELD_SEP;
                kv += f[i];
            }
            sl_session_meta_t meta;
            int sessionRc = sl_session_meta_parse_kv_list(
                kv.c_str(), SL_IPC_FIELD_SEP, &meta);
            sl_sync_meta_t syncMeta;
            int syncRc = sl_sync_meta_parse_kv_list(kv.c_str(),
                                                   SL_IPC_FIELD_SEP, &syncMeta);
            if (sessionRc < 0 || syncRc < 0 ||
                (sessionRc != 1 && syncRc != 1)) {
                sl::DebugLogContext context;
                context.generation = gen;
                context.requestId = req;
                daemonLogContext(context,
                                 "bad meta fields session_rc=%d sync_rc=%d",
                                 sessionRc, syncRc);
                continue;
            }
            sl::DebugLogContext context;
            context.generation = gen;
            context.requestId = req;
            if (sessionRc == 1) {
                storeSessionMeta(gen, meta);
                context = debugContextFromMeta(gen, meta, req);
                daemonLogContext(context, "meta ok %s",
                                 metaIdentity(meta).c_str());
            }
            if (syncRc == 1) {
                {
                    std::lock_guard<std::mutex> lock(g_syncContextMutex);
                    sl_sync_context_set(&g_pendingSync, gen, req, &syncMeta);
                }
                daemonLogContext(context,
                                 "sync meta pending epoch=%llu target_ms=%lld "
                                 "reason=%s desired=%s",
                                 (unsigned long long)syncMeta.epoch,
                                 (long long)syncMeta.target_ms, syncMeta.reason,
                                 syncMeta.desired);
            }
            continue;
        }

        if (cmd == SL_CMD_OPEN) {
            /* open<gen><req><videoUrl><referrer><start><end><audioUrls...> */
            if (f.size() < 7) {
                emit(SL_EVT_ERROR, "bad open command");
                continue;
            }
            std::uint64_t fileGen = parseUnsigned(f[1]);
            std::uint64_t requestId = parseUnsigned(f[2]);
            g_fileGeneration.store(fileGen);
            sl_session_meta_t storedMeta;
            sl::DebugLogContext context;
            if (getSessionMeta(fileGen, storedMeta))
                context = debugContextFromMeta(fileGen, storedMeta, requestId);
            else {
                context.generation = fileGen;
                context.requestId = requestId;
            }
            sl_sync_meta_t openSync;
            bool hasOpenSync =
                consumeSyncContext(fileGen, requestId, context, &openSync);
            sl::setDebugLogContext(context);
            std::lock_guard<std::mutex> lock(playerMutex);
            daemonLogReq(requestId, "open enter opened=%d", opened ? 1 : 0);
            if (opened) player.stop();
            daemonLogReq(requestId, "open old player stopped");
            sl::SourceSpec spec;
            spec.debug = context;
            if (hasOpenSync)
                spec.sync = syncContextFromMeta(openSync, requestId);
            spec.videoUrl = f[3];
            spec.referrer = f[4];
            spec.startSeconds = atof(f[5].c_str());
            spec.endSeconds = atof(f[6].c_str());
            for (size_t i = 7; i < f.size(); ++i) {
                if (!f[i].empty()) spec.audioUrls.push_back(f[i]);
            }
            lastProgressMs.store(0);
            daemonLogReq(requestId, "open source audio=%zu", spec.audioUrls.size());
            if (!player.open(spec)) {
                daemonLogReq(requestId, "open failed");
                emitContext(SL_EVT_ERROR, requestId, "SlPlayer open failed");
                continue;
            }
            opened = true;
            daemonLogReq(requestId, "open ok play");
            player.play(0);
        } else if (cmd == SL_CMD_PAUSE) {
            std::uint64_t req = parseUnsigned(f[2]);
            std::uint64_t gen = parseUnsigned(f[1]);
            sl::DebugLogContext context = sl::currentDebugLogContext();
            context.generation = gen;
            context.requestId = req;
            sl_sync_meta_t pauseSync;
            bool hasPauseSync = consumeSyncContext(gen, req, context, &pauseSync);
            if (commandGenerationMatches(f) && opened) {
                sl::setDebugLogContext(context);
                sl::SyncContext sync;
                if (hasPauseSync) sync = syncContextFromMeta(pauseSync, req);
                daemonLogReq(req, "pause status=sent");
                player.pause(req, sync);
            }
        } else if (cmd == SL_CMD_RESUME) {
            std::uint64_t req = parseUnsigned(f[2]);
            std::uint64_t gen = parseUnsigned(f[1]);
            sl::DebugLogContext context = sl::currentDebugLogContext();
            context.generation = gen;
            context.requestId = req;
            sl_sync_meta_t resumeSync;
            bool hasResumeSync = consumeSyncContext(gen, req, context, &resumeSync);
            if (commandGenerationMatches(f) && opened) {
                sl::setDebugLogContext(context);
                sl::SyncContext sync;
                if (hasResumeSync) sync = syncContextFromMeta(resumeSync, req);
                daemonLogReq(req, "resume status=sent");
                player.play(req, sync);
            }
        } else if (cmd == SL_CMD_SEEK) {
            std::uint64_t req = parseUnsigned(f[2]);
            std::uint64_t gen = parseUnsigned(f[1]);
            sl::DebugLogContext context = sl::currentDebugLogContext();
            context.generation = gen;
            context.requestId = req;
            sl_sync_meta_t seekSync;
            bool hasSeekSync = consumeSyncContext(gen, req, context, &seekSync);
            if (commandGenerationMatches(f) && opened && f.size() >= 4) {
                sl::setDebugLogContext(context);
                sl::SyncContext sync;
                if (hasSeekSync) sync = syncContextFromMeta(seekSync, req);
                daemonLogReq(req, "seek target=%.3f status=sent", atof(f[3].c_str()));
                player.seek(atof(f[3].c_str()), req, sync);
            }
        } else if (cmd == SL_CMD_STOP) {
            if (!commandGenerationMatches(f)) continue;
            std::lock_guard<std::mutex> lock(playerMutex);
            if (opened) {
                daemonLogReq(parseUnsigned(f[2]), "stop enter");
                player.stop();
                daemonLogReq(parseUnsigned(f[2]), "stop done");
                opened = false;
            }
        } else if (cmd == SL_CMD_VOLUME) {
            if (commandGenerationMatches(f) && f.size() >= 4)
                player.setVolume(atoi(f[3].c_str()));
        } else if (cmd == SL_CMD_HEARTBEAT) {
            if (!commandGenerationMatches(f)) continue;
            sl::Heartbeat hb = player.heartbeat();
            char args[400];
            snprintf(args, sizeof(args),
                     "%lld\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%u\t%lld\t%d",
                     (long long)hb.lastFrameTimeMs, hb.stage,
                     hb.playing ? 1 : 0, hb.inTransition ? 1 : 0,
                     hb.videoFrames, hb.currentPaused ? 1 : 0,
                     hb.targetPaused ? 1 : 0, hb.seeking ? 1 : 0,
                     hb.workerAlive ? 1 : 0, hb.presentationMs, hb.videoQueueMs,
                     (long long)hb.networkErrorAgeMs, hb.backpressure ? 1 : 0);
            emitContext(SL_EVT_HEARTBEAT, parseUnsigned(f[2]), args);
        } else if (cmd == SL_CMD_QUIT) {
            daemonLog("quit 命令");
            break;
        } else {
            daemonLog("未知命令: %s", cmd.c_str());
        }
    }

    daemonLog("shutdown");
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (opened) {
            daemonLog("shutdown stop enter");
            player.stop();
            daemonLog("shutdown stop done");
        }
    }
    if (g_clientFd >= 0) close(g_clientFd);
    if (g_listenFd >= 0) close(g_listenFd);
    unlink(path);
    daemonLog("exited");
    return 0;
}
