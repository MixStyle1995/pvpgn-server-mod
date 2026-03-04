/*
 * d2fs_server.cpp — D2FileServer tích hợp vào d2dbs
 *
 * Kiến trúc : thread pool (WORKERS) + 1 accept thread
 * Whitelist : d2dbs_prefs_get_d2gs_list()
 * Firewall  : tự động mở/đóng khi start/stop
 *             Linux   → iptables / firewall-cmd / ufw
 *             Windows → netsh advfirewall
 *
 * Cấu trúc thư mục:
 *   <datadir>/d2fs/
 *     charsave/<acc>/<chr>.d2s
 *     charstash/<acc>/<chr>.stash
 *     charcube/<acc>/<chr>.cube
 *     charsave_ex/<acc>/<chr>.ex
 *
 * Protocol : [uint32 size][uint8 cmd][uint8 pad[3]][payload...]
 *   0x01 SaveChar    [acc_len][chr_len][acc][chr][uint32 datalen][data]
 *   0x02 LoadChar    [acc_len][chr_len][acc][chr]
 *   0x03 SaveStash   (same layout as SaveChar)
 *   0x04 LoadStash   (same layout as LoadChar)
 *   0x05 SaveCube    (same layout as SaveChar)
 *   0x06 LoadCube    (same layout as LoadChar)
 *   0x07 SaveCharEx  (same layout as SaveChar)
 *   0x08 LoadCharEx  (same layout as LoadChar)
 *   0x80 Reply       [orig_cmd][result][pad2][uint32 datalen][data...]
 *        result: 0 = OK | 1 = NOT_FOUND | 2 = ERROR
 */

#include "common/setup_before.h"
#include "setup.h"
#include "d2fs_server.h"
#include "common/eventlog.h"
#include "prefs.h"
#include "common/setup_after.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#ifdef WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <mstcpip.h>
#  include <direct.h>
#  include <io.h>
typedef SOCKET sock_t;
#  define SOCK_INVALID  INVALID_SOCKET
#  define sock_close    closesocket
#  define os_mkdir(p)   _mkdir(p)
#  define os_access(p)  _access(p, 0)
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <sys/stat.h>
typedef int sock_t;
#  define SOCK_INVALID  (-1)
#  define sock_close    close
#  define os_mkdir(p)   mkdir(p, 0755)
#  define os_access(p)  access(p, F_OK)
#endif

// ─── Tuning ───────────────────────────────────────────────────────────────────

static const int WORKERS = 32;
static const int QUEUE_MAX = 2048;

// ─── Protocol ─────────────────────────────────────────────────────────────────

static const uint32_t HDR_SIZE = 8;
static const uint32_t MAX_ACC = 31;
static const uint32_t MAX_CHR = 15;
static const uint32_t MAX_PKT = 8 * 1024 * 1024;  // 8 MB

enum Cmd : uint8_t
{
    CMD_SAVE_CHAR = 0x01,
    CMD_LOAD_CHAR = 0x02,
    CMD_SAVE_STASH = 0x03,
    CMD_LOAD_STASH = 0x04,
    CMD_SAVE_CUBE = 0x05,
    CMD_LOAD_CUBE = 0x06,
    CMD_SAVE_CHAR_EX = 0x07,
    CMD_LOAD_CHAR_EX = 0x08,
    CMD_REPLY = 0x80,
};

enum Result : uint8_t
{
    RES_OK = 0,
    RES_NOT_FOUND = 1,
    RES_ERROR = 2,
};

#pragma pack(1)
struct PacketHdr
{
    uint32_t size;
    uint8_t  cmd;
    uint8_t  pad[3];
};
struct ReplyHdr
{
    PacketHdr hdr;
    uint8_t   orig_cmd;
    uint8_t   result;
    uint8_t   pad[2];
    uint32_t  datalen;
};
#pragma pack()

namespace pvpgn
{
    namespace d2dbs
    {

        // ─── Logging ──────────────────────────────────────────────────────────────────

        static std::mutex g_log_mtx;

        template <typename... Args>
        static void log(t_eventlog_level level, const char* fmt, Args&&... args)
        {
            std::lock_guard<std::mutex> lk(g_log_mtx);
            eventlog(level, "d2fs", fmt, std::forward<Args>(args)...);
        }

        // ─── File I/O ─────────────────────────────────────────────────────────────────

        static void make_dirs(const std::string& path)
        {
            for (size_t i = 1; i < path.size(); ++i)
            {
                if (path[i] == '/' || path[i] == '\\')
                {
                    std::string sub = path.substr(0, i);
                    if (os_access(sub.c_str()) != 0)
                        os_mkdir(sub.c_str());
                }
            }
            if (os_access(path.c_str()) != 0)
                os_mkdir(path.c_str());
        }

        static std::string sanitize(const char* s, size_t n)
        {
            std::string out(s, n);
            for (auto& c : out)
                c = (isalnum((uint8_t)c) || c == '-' || c == '_')
                ? (char)tolower((uint8_t)c)
                : '_';
            return out;
        }

        static bool write_file(const std::string& path, const uint8_t* data, uint32_t size)
        {
            std::string tmp = path + ".tmp";

            FILE* f = fopen(tmp.c_str(), "wb");
            if (!f) return false;

            bool ok = (fwrite(data, 1, size, f) == size);
            fclose(f);

            if (!ok) { remove(tmp.c_str()); return false; }
#ifdef WIN32
            remove(path.c_str());
#endif
            return rename(tmp.c_str(), path.c_str()) == 0;
        }

        static std::vector<uint8_t> read_file(const std::string& path)
        {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) return {};

            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            rewind(f);

            if (sz <= 0) { fclose(f); return {}; }

            std::vector<uint8_t> buf(sz);
            bool ok = (fread(buf.data(), 1, sz, f) == (size_t)sz);
            fclose(f);

            return ok ? buf : std::vector<uint8_t>{};
        }

        // ─── Path resolver ────────────────────────────────────────────────────────────
        // Mỗi cmd có thư mục và đuôi file riêng

        struct FileType
        {
            const char* subdir;
            const char* ext;
        };

        static FileType get_file_type(uint8_t cmd)
        {
            switch (cmd)
            {
                case CMD_SAVE_CHAR:
                case CMD_LOAD_CHAR:    return { "charsave",    ".d2s" };
                case CMD_SAVE_STASH:
                case CMD_LOAD_STASH:   return { "charstash",   ".stash" };
                case CMD_SAVE_CUBE:
                case CMD_LOAD_CUBE:    return { "charcube",    ".cube" };
                case CMD_SAVE_CHAR_EX:
                case CMD_LOAD_CHAR_EX: return { "charsave_ex", ".ex" };
                default:               return { "",            "" };
            }
        }

        static std::string build_path(const std::string& datadir, uint8_t cmd, const std::string& acc, const std::string& chr)
        {
            auto ft = get_file_type(cmd);
            return datadir + "/d2fs/" + ft.subdir + "/" + acc + "/" + chr + ft.ext;
        }

        static const char* cmd_name(uint8_t cmd)
        {
            switch (cmd)
            {
            case CMD_SAVE_CHAR:    return "SaveChar";
            case CMD_LOAD_CHAR:    return "LoadChar";
            case CMD_SAVE_STASH:   return "SaveStash";
            case CMD_LOAD_STASH:   return "LoadStash";
            case CMD_SAVE_CUBE:    return "SaveCube";
            case CMD_LOAD_CUBE:    return "LoadCube";
            case CMD_SAVE_CHAR_EX: return "SaveCharEx";
            case CMD_LOAD_CHAR_EX: return "LoadCharEx";
            default:               return "Unknown";
            }
        }

        static bool is_save_cmd(uint8_t cmd)
        {
            return cmd == CMD_SAVE_CHAR || cmd == CMD_SAVE_STASH
                || cmd == CMD_SAVE_CUBE || cmd == CMD_SAVE_CHAR_EX;
        }

        static bool is_load_cmd(uint8_t cmd)
        {
            return cmd == CMD_LOAD_CHAR || cmd == CMD_LOAD_STASH
                || cmd == CMD_LOAD_CUBE || cmd == CMD_LOAD_CHAR_EX;
        }

        // ─── IP Whitelist ─────────────────────────────────────────────────────────────

        static bool verify_ip(uint32_t ip_net)
        {
            const char* list = d2dbs_prefs_get_d2gs_list();
            if (!list || !*list) return false;

            char* buf = strdup(list);
            bool  allowed = false;
            char* tok = buf;
            char* entry;

#ifdef WIN32
            char* ctx = nullptr;
            while ((entry = strtok_s(tok, ",", &ctx)), tok = nullptr, entry)
            {
#else
            while ((entry = strsep(&tok, ",")))
            {
#endif
                while (*entry == ' ' || *entry == '\t') entry++;
                char* end = entry + strlen(entry) - 1;
                while (end > entry && (*end == ' ' || *end == '\t')) *end-- = '\0';

                if (!*entry) continue;

                addrinfo hints{}, * res = nullptr;
                hints.ai_family = AF_INET;

                if (getaddrinfo(entry, nullptr, &hints, &res) == 0 && res)
                {
                    uint32_t resolved = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
                    freeaddrinfo(res);
                    if (resolved == ip_net) { allowed = true; break; }
                }
                else if (res)
                {
                    freeaddrinfo(res);
                }
            }

            free(buf);
            return allowed;
            }

        // ─── Firewall ─────────────────────────────────────────────────────────────────

        static void run_cmd(const char* cmd)
        {
            log(eventlog_level_info, "firewall: {}", cmd);
            system(cmd);
        }

#ifdef WIN32

        static void firewall_open(uint16_t port)
        {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "netsh advfirewall firewall add rule" " name=\"d2fs_%u\" protocol=TCP dir=in action=allow localport=%u >nul 2>&1", port, port);
            run_cmd(cmd);
        }

        static void firewall_close(uint16_t port)
        {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "netsh advfirewall firewall delete rule name=\"d2fs_%u\" >nul 2>&1", port);
            run_cmd(cmd);
        }

#else

        static bool has_tool(const char* name)
        {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
            return system(cmd) == 0;
        }

        static void firewall_open(uint16_t port)
        {
            char cmd[512];

            if (has_tool("iptables"))
            {
                snprintf(cmd, sizeof(cmd), "iptables -C INPUT -p tcp --dport %u -j ACCEPT 2>/dev/null" " || iptables -I INPUT -p tcp --dport %u -j ACCEPT", port, port);
            }
            else if (has_tool("firewall-cmd"))
            {
                snprintf(cmd, sizeof(cmd), "firewall-cmd --permanent --add-port=%u/tcp >/dev/null 2>&1" " && firewall-cmd --reload >/dev/null 2>&1", port);
            }
            else if (has_tool("ufw"))
            {
                snprintf(cmd, sizeof(cmd), "ufw allow %u/tcp >/dev/null 2>&1", port);
            }
            else
            {
                log(eventlog_level_warn, "no firewall tool found, port {} may be blocked", port);
                return;
            }

            run_cmd(cmd);
        }

        static void firewall_close(uint16_t port)
        {
            char cmd[512];

            if (has_tool("iptables"))
            {
                snprintf(cmd, sizeof(cmd), "while iptables -C INPUT -p tcp --dport %u -j ACCEPT 2>/dev/null;" " do iptables -D INPUT -p tcp --dport %u -j ACCEPT; done", port, port);
            }
            else if (has_tool("firewall-cmd"))
            {
                snprintf(cmd, sizeof(cmd), "firewall-cmd --permanent --remove-port=%u/tcp >/dev/null 2>&1" " && firewall-cmd --reload >/dev/null 2>&1", port);
            }
            else if (has_tool("ufw"))
            {
                snprintf(cmd, sizeof(cmd), "ufw delete allow %u/tcp >/dev/null 2>&1", port);
            }
            else
            {
                return;
            }

            run_cmd(cmd);
        }

#endif

        // ─── TCP helpers ──────────────────────────────────────────────────────────────

        static bool tcp_recv(sock_t s, void* buf, size_t len)
        {
            char* p = (char*)buf;
            while (len > 0)
            {
                int n = recv(s, p, (int)len, 0);
                if (n <= 0) return false;
                p += n; len -= n;
            }
            return true;
        }

        static bool tcp_send(sock_t s, const void* buf, size_t len)
        {
            const char* p = (const char*)buf;
            while (len > 0)
            {
                int n = send(s, p, (int)len, 0);
                if (n <= 0) return false;
                p += n; len -= n;
            }
            return true;
        }

        static bool send_reply(sock_t s, uint8_t cmd, uint8_t result, const uint8_t * data = nullptr, uint32_t datalen = 0)
        {
            std::vector<uint8_t> pkt(sizeof(ReplyHdr) + datalen, 0);
            auto* r = (ReplyHdr*)pkt.data();
            r->hdr.size = (uint32_t)pkt.size();
            r->hdr.cmd = CMD_REPLY;
            r->orig_cmd = cmd;
            r->result = result;
            r->datalen = datalen;
            if (data && datalen)
                memcpy(pkt.data() + sizeof(ReplyHdr), data, datalen);
            return tcp_send(s, pkt.data(), pkt.size());
        }

        static bool parse_names(const uint8_t * p, size_t ps, std::string & acc, std::string & chr, size_t * consumed = nullptr)
        {
            if (ps < 2) return false;
            uint8_t al = p[0], cl = p[1];
            if (!al || al > MAX_ACC || !cl || cl > MAX_CHR) return false;
            if (ps < (size_t)(2 + al + cl)) return false;
            acc = sanitize((const char*)p + 2, al);
            chr = sanitize((const char*)p + 2 + al, cl);
            if (consumed) *consumed = 2 + al + cl;
            return true;
        }

        // ─── Thread Pool ──────────────────────────────────────────────────────────────

        struct Job
        {
            sock_t      sd;
            uint32_t    ip_net;
            std::string peer;
        };

        struct ThreadPool
        {
            std::vector<std::thread>  workers;
            std::queue<Job>           queue;
            std::mutex                mtx;
            std::condition_variable   cv;
            std::atomic<bool>         stopped{ false };
            std::string               datadir;

            void start(int n, std::string dir)
            {
                datadir = std::move(dir);
                for (int i = 0; i < n; ++i)
                    workers.emplace_back([this] { worker_loop(); });
                log(eventlog_level_info, "pool started ({} workers)", n);
            }

            void enqueue(Job job)
            {
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    if ((int)queue.size() >= QUEUE_MAX)
                    {
                        log(eventlog_level_warn, "queue full, dropping {}", job.peer.c_str());
                        sock_close(job.sd);
                        return;
                    }
                    queue.push(std::move(job));
                }
                cv.notify_one();
            }

            void shutdown()
            {
                stopped = true;
                cv.notify_all();
                for (auto& t : workers)
                    if (t.joinable()) t.join();
                std::lock_guard<std::mutex> lk(mtx);
                while (!queue.empty())
                {
                    sock_close(queue.front().sd);
                    queue.pop();
                }
            }

        private:
            void worker_loop()
            {
                for (;;)
                {
                    Job job;
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cv.wait(lk, [this] { return stopped.load() || !queue.empty(); });
                        if (stopped && queue.empty()) return;
                        job = std::move(queue.front());
                        queue.pop();
                    }
                    handle(job);
                }
            }

            void handle(const Job& job)
            {
                sock_t s = job.sd;

                if (!verify_ip(job.ip_net))
                {
                    log(eventlog_level_error, "rejected: {}", job.peer.c_str());
                    sock_close(s);
                    return;
                }

                log(eventlog_level_info, "connected: {}", job.peer.c_str());

                for (;;)
                {
                    PacketHdr hdr;
                    if (!tcp_recv(s, &hdr, sizeof(hdr))) break;

                    if (hdr.size < HDR_SIZE || hdr.size > MAX_PKT)
                    {
                        log(eventlog_level_error, "[{}] invalid size {}", job.peer.c_str(), hdr.size);
                        break;
                    }

                    std::vector<uint8_t> payload(hdr.size - HDR_SIZE);
                    if (!payload.empty() && !tcp_recv(s, payload.data(), payload.size())) break;

                    const uint8_t* p = payload.data();
                    size_t         ps = payload.size();
                    std::string    acc, chr;

                    if (is_save_cmd(hdr.cmd))
                    {
                        size_t   nb = 0;
                        uint32_t dlen = 0;

                        if (!parse_names(p, ps, acc, chr, &nb) || ps < nb + 4)
                        {
                            send_reply(s, hdr.cmd, RES_ERROR); continue;
                        }
                        memcpy(&dlen, p + nb, 4);
                        if (ps < nb + 4 + dlen)
                        {
                            send_reply(s, hdr.cmd, RES_ERROR); continue;
                        }

                        std::string path = build_path(datadir, hdr.cmd, acc, chr);
                        auto        slash = path.rfind('/');
                        if (slash != std::string::npos)
                            make_dirs(path.substr(0, slash));

                        bool ok = write_file(path, p + nb + 4, dlen);
                        log(ok ? eventlog_level_info : eventlog_level_error, "[{}] {} {}/{} len={} {}", job.peer.c_str(), cmd_name(hdr.cmd), acc.c_str(), chr.c_str(), dlen, ok ? "ok" : "FAILED");
                        send_reply(s, hdr.cmd, ok ? RES_OK : RES_ERROR);
                    }
                    else if (is_load_cmd(hdr.cmd))
                    {
                        if (!parse_names(p, ps, acc, chr))
                        {
                            send_reply(s, hdr.cmd, RES_ERROR); continue;
                        }

                        std::string path = build_path(datadir, hdr.cmd, acc, chr);
                        auto        data = read_file(path);

                        if (data.empty())
                        {
                            log(eventlog_level_warn, "[{}] {} {}/{} NOT_FOUND", job.peer.c_str(), cmd_name(hdr.cmd), acc.c_str(), chr.c_str());
                            send_reply(s, hdr.cmd, RES_NOT_FOUND);
                        }
                        else
                        {
                            log(eventlog_level_info, "[{}] {} {}/{} len={} ok", job.peer.c_str(), cmd_name(hdr.cmd), acc.c_str(), chr.c_str(), (unsigned)data.size());
                            send_reply(s, hdr.cmd, RES_OK, data.data(), (uint32_t)data.size());
                        }
                    }
                    else
                    {
                        log(eventlog_level_warn, "[{}] unknown cmd 0x{:02X}", job.peer.c_str(), hdr.cmd);
                        send_reply(s, hdr.cmd, RES_ERROR);
                    }
                }

                sock_close(s);
                log(eventlog_level_info, "disconnected: {}", job.peer.c_str());
            }
        };

        // ─── Server state ─────────────────────────────────────────────────────────────

        static ThreadPool        g_pool;
        static std::atomic<bool> g_running{ false };
        static std::thread       g_accept_thread;
        static sock_t            g_listen = SOCK_INVALID;
        static uint16_t          g_port = 0;

        // ─── Accept loop ──────────────────────────────────────────────────────────────

        static void accept_loop(uint16_t port)
        {
            g_listen = (sock_t)socket(AF_INET, SOCK_STREAM, 0);
            if (g_listen == SOCK_INVALID)
            {
                log(eventlog_level_error, "socket() failed");
                return;
            }

            int yes = 1;
            setsockopt((int)g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

            // Tắt Nagle — giảm latency cho packet nhỏ (header 8 bytes)
            setsockopt((int)g_listen, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind((int)g_listen, (sockaddr*)&addr, sizeof(addr)) < 0 || listen((int)g_listen, QUEUE_MAX) < 0)
            {
                log(eventlog_level_error, "bind/listen failed on port {}", port);
                sock_close(g_listen);
                g_listen = SOCK_INVALID;
                return;
            }

            log(eventlog_level_info, "listening on port {} (workers={} queue={})", port, WORKERS, QUEUE_MAX);

            while (g_running.load())
            {
                fd_set  rset;
                timeval tv{ 1, 0 };
                FD_ZERO(&rset);
                FD_SET((unsigned)g_listen, &rset);

                if (select((int)g_listen + 1, &rset, nullptr, nullptr, &tv) <= 0)
                    continue;

                sockaddr_in caddr{};
                socklen_t   clen = sizeof(caddr);
                sock_t      cs = (sock_t)accept((int)g_listen, (sockaddr*)&caddr, &clen);

                if (cs == SOCK_INVALID) continue;

                // TCP_NODELAY — không gộp packet nhỏ, reply ngay lập tức
                setsockopt((int)cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes));

                // SO_KEEPALIVE — phát hiện client chết (không gửi FIN)
                setsockopt((int)cs, SOL_SOCKET, SO_KEEPALIVE, (const char*)&yes, sizeof(yes));

#ifdef WIN32
                // Keepalive: bắt đầu sau 60s idle, probe mỗi 10s, 3 lần thất bại thì close
                struct tcp_keepalive ka { 1, 60000, 10000 };
                DWORD bytes = 0;
                WSAIoctl(cs, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &bytes, nullptr, nullptr);
#elif defined(__linux__)
                int ka_idle = 60, ka_interval = 10, ka_count = 3;
                setsockopt(cs, IPPROTO_TCP, TCP_KEEPIDLE, &ka_idle, sizeof(ka_idle));
                setsockopt(cs, IPPROTO_TCP, TCP_KEEPINTVL, &ka_interval, sizeof(ka_interval));
                setsockopt(cs, IPPROTO_TCP, TCP_KEEPCNT, &ka_count, sizeof(ka_count));
#endif

                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
                std::string peer = std::string(ip) + ":" + std::to_string(ntohs(caddr.sin_port));

                g_pool.enqueue({ cs, caddr.sin_addr.s_addr, std::move(peer) });
            }

            sock_close(g_listen);
            g_listen = SOCK_INVALID;
            log(eventlog_level_info, "accept loop stopped");
        }

        // ─── Public API ───────────────────────────────────────────────────────────────

        void d2fs_server_start(unsigned short port, const char* datadir)
        {
            if (g_running.load()) return;

            g_port = (uint16_t)port;
            std::string dir = datadir ? datadir : "./d2fsdata";

            // Tạo tất cả thư mục cần thiết
            make_dirs(dir + "/d2fs/charsave");
            make_dirs(dir + "/d2fs/charstash");
            make_dirs(dir + "/d2fs/charcube");
            make_dirs(dir + "/d2fs/charsave_ex");

            firewall_open(g_port);
            g_pool.start(WORKERS, dir);

            g_running.store(true);
            g_accept_thread = std::thread(accept_loop, g_port);

            log(eventlog_level_info, "started on port {} (datadir={})", port, dir.c_str());
        }

        void d2fs_server_stop(void)
        {
            if (!g_running.load()) return;

            g_running.store(false);
            if (g_accept_thread.joinable())
                g_accept_thread.join();

            g_pool.shutdown();
            firewall_close(g_port);

            log(eventlog_level_info, "stopped");
        }

     } // namespace d2dbs
} // namespace pvpgn