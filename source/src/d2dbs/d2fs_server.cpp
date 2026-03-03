/*
 * d2fs_server.cpp  —  D2FileServer tích hợp vào d2dbs
 *
 * Kiến trúc: single thread + select() + non-blocking I/O
 * Giống hệt d2dbs gốc — không dùng thread pool hay spawn thread per connection.
 * select() loop xử lý tất cả connection trong 1 thread duy nhất.
 *
 * Mỗi connection có read buffer riêng. Khi buffer đủ 1 packet hoàn chỉnh
 * thì mới xử lý — không bao giờ block chờ data.
 *
 * Disk write (write_atomic) là blocking → chạy trên 1 write worker thread
 * riêng để không block select() loop. Read file nhỏ thì đọc inline vì nhanh.
 *
 * Protocol: [uint32 size][uint8 cmd][uint8 pad[3]][payload...]
 *   CMD_SAVE_CHAR  0x01  [acc_len][chr_len][acc][chr][uint32 datalen][data]
 *   CMD_LOAD_CHAR  0x02  [acc_len][chr_len][acc][chr]
 *   CMD_SAVE_STASH 0x03  [acc_len][chr_len][acc][chr][uint32 datalen][data]
 *   CMD_LOAD_STASH 0x04  [acc_len][chr_len][acc][chr]
 *   CMD_REPLY      0x80  [orig_cmd][result][pad2][uint32 datalen][data...]
 *   result: 0=OK  1=NOT_FOUND  2=ERROR
 */

#include "common/setup_before.h"
#include "setup.h"
#include "d2fs_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include <list>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <algorithm>

#ifdef WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <direct.h>
#  include <io.h>
typedef SOCKET d2fs_sock_t;
#  define D2FS_INVALID_SOCK  INVALID_SOCKET
#  define D2FS_SOCK_ERR(s)   ((s) == INVALID_SOCKET)
#  define D2FS_CLOSE(s)      closesocket(s)
#  define d2fs_mkdir(p)      _mkdir(p)
#  define d2fs_access(p)     _access(p, 0)
#  define D2FS_NONBLOCK(s)   { u_long _m=1; ioctlsocket(s,FIONBIO,&_m); }
#  define D2FS_WOULDBLOCK    (WSAGetLastError()==WSAEWOULDBLOCK)
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <errno.h>
typedef int d2fs_sock_t;
#  define D2FS_INVALID_SOCK  (-1)
#  define D2FS_SOCK_ERR(s)   ((s) < 0)
#  define D2FS_CLOSE(s)      close(s)
#  define d2fs_mkdir(p)      mkdir(p, 0755)
#  define d2fs_access(p)     access(p, F_OK)
#  define D2FS_NONBLOCK(s)   fcntl(s, F_SETFL, fcntl(s,F_GETFL,0)|O_NONBLOCK)
#  define D2FS_WOULDBLOCK    (errno==EAGAIN || errno==EWOULDBLOCK)
#endif

#include "common/eventlog.h"
#include "prefs.h"
#include "common/setup_after.h"

// ─── Protocol ─────────────────────────────────────────────────────────────────
#define D2FS_HDR_SIZE        8u
#define D2FS_MAX_ACC         31u
#define D2FS_MAX_CHR         15u
#define D2FS_MAX_PKT         (8u * 1024u * 1024u)  // 8MB max packet
#define D2FS_READ_BUF_SIZE   (D2FS_MAX_PKT + D2FS_HDR_SIZE)
#define D2FS_WRITE_BUF_SIZE  (D2FS_MAX_PKT + 32u)
#define D2FS_SELECT_TIMEOUT  100000  // 100ms — giống SELECT_TIME_OUT của d2dbs

#define D2FS_CMD_SAVE_CHAR   0x01
#define D2FS_CMD_LOAD_CHAR   0x02
#define D2FS_CMD_SAVE_STASH  0x03
#define D2FS_CMD_LOAD_STASH  0x04
#define D2FS_CMD_REPLY       0x80

#define D2FS_OK         0
#define D2FS_NOT_FOUND  1
#define D2FS_ERROR      2

#pragma pack(push, 1)
struct d2fs_hdr {
    uint32_t size;
    uint8_t  cmd;
    uint8_t  pad[3];
};
struct d2fs_reply_hdr {
    d2fs_hdr hdr;
    uint8_t  orig_cmd;
    uint8_t  result;
    uint8_t  pad2[2];
    uint32_t datalen;
};
#pragma pack(pop)

namespace pvpgn {
    namespace d2dbs {

        // ─── Connection state — giống t_d2dbs_connection của d2dbs ───────────────────
        struct D2FSConn {
            d2fs_sock_t  sd;
            uint32_t     ip_net;
            std::string  peer;
            bool         verified;
            std::time_t  last_active;

            // Read buffer — tích lũy data cho đến khi đủ 1 packet
            std::vector<uint8_t> rbuf;
            size_t               rbuf_len;   // bytes hiện có trong rbuf

            // Write buffer — data chờ gửi (non-blocking write có thể gửi không hết)
            std::vector<uint8_t> wbuf;
            size_t               wbuf_len;
            size_t               wbuf_sent;

            D2FSConn(d2fs_sock_t s, uint32_t ip, const std::string& p)
                : sd(s), ip_net(ip), peer(p), verified(false),
                last_active(std::time(nullptr)),
                rbuf(D2FS_READ_BUF_SIZE), rbuf_len(0),
                wbuf(D2FS_WRITE_BUF_SIZE), wbuf_len(0), wbuf_sent(0)
            {
            }
        };

        // ─── Write job — giao cho write worker thread ─────────────────────────────────
        // Disk write là blocking I/O, không thể làm trong select() loop.
        // Sau khi write xong, reply được đưa vào wbuf của connection để gửi.
        struct WriteJob {
            std::string              fpath;
            std::vector<uint8_t>     data;
            uint8_t                  cmd;
            d2fs_sock_t              sd;     // để tìm lại connection
            std::string              log_str;
        };

        struct WriteWorker {
            std::thread             thread;
            std::queue<WriteJob>    queue;
            std::mutex              mtx;
            std::condition_variable cv;
            std::atomic<bool>       stop{ false };

            // Completed results — select() loop poll và gửi reply
            struct Result {
                d2fs_sock_t sd;
                uint8_t     cmd;
                uint8_t     result;
                std::string log_str;
            };
            std::queue<Result>  results;
            std::mutex              results_mtx;

            void start();
            void push(WriteJob job);
            void shutdown();
            bool pop_result(Result& r);
        };

        // ─── State ────────────────────────────────────────────────────────────────────
        static std::atomic<bool>    s_running{ false };
        static std::thread          s_server_thread;
        static d2fs_sock_t          s_listen_sock = D2FS_INVALID_SOCK;
        static std::string          s_datadir;
        static unsigned short       s_port = 0;
        static WriteWorker          s_writer;
        static std::list<D2FSConn>  s_conns;  // danh sách connection active

        // ─── Thread-safe log ──────────────────────────────────────────────────────────
        static std::mutex s_log_mtx;
#define D2FS_LOG(level, fmt, ...) \
    do { std::lock_guard<std::mutex> _lk(s_log_mtx); \
         eventlog(level, "d2fs", fmt, ##__VA_ARGS__); } while(0)

        // ─── File helpers ─────────────────────────────────────────────────────────────
        static void make_dirs(const std::string& path)
        {
            for (size_t i = 1; i < path.size(); ++i) {
                if (path[i] == '/' || path[i] == '\\') {
                    std::string sub = path.substr(0, i);
                    if (d2fs_access(sub.c_str()) != 0) d2fs_mkdir(sub.c_str());
                }
            }
            if (d2fs_access(path.c_str()) != 0) d2fs_mkdir(path.c_str());
        }

        static std::string sanitize(const char* s, size_t len)
        {
            std::string out(s, len);
            for (auto& c : out) {
                c = (char)tolower((unsigned char)c);
                if (!isalnum((unsigned char)c) && c != '_' && c != '-') c = '_';
            }
            return out;
        }

        static std::string char_path(const std::string& acc, const std::string& chr)
        {
            return s_datadir + "/charsave/" + acc + "/" + chr + ".d2s";
        }
        static std::string stash_path(const std::string& acc, const std::string& chr)
        {
            return s_datadir + "/charstash/" + acc + "/" + chr + ".stash";
        }

        static bool write_atomic(const std::string& path, const uint8_t* data, uint32_t len)
        {
            std::string tmp = path + ".tmp";
            FILE* f = fopen(tmp.c_str(), "wb");
            if (!f) return false;
            bool ok = (fwrite(data, 1, len, f) == len);
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
            long sz = ftell(f); rewind(f);
            if (sz <= 0) { fclose(f); return {}; }
            std::vector<uint8_t> buf((size_t)sz);
            if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return {}; }
            fclose(f);
            return buf;
        }

        static bool parse_names(const uint8_t* p, size_t ps,
            std::string& acc, std::string& chr, size_t* consumed)
        {
            if (ps < 2) return false;
            uint8_t al = p[0], cl = p[1];
            if (!al || al > D2FS_MAX_ACC || !cl || cl > D2FS_MAX_CHR) return false;
            if (ps < (size_t)(2 + al + cl)) return false;
            acc = sanitize((const char*)p + 2, al);
            chr = sanitize((const char*)p + 2 + al, cl);
            if (consumed) *consumed = 2 + al + cl;
            return true;
        }

        // ─── IP whitelist ─────────────────────────────────────────────────────────────
        static bool d2fs_verify_ip(uint32_t ip_net)
        {
            const char* list = d2dbs_prefs_get_d2gs_list();
            if (!list || !list[0]) return false;
            char* buf = (char*)malloc(strlen(list) + 1);
            if (!buf) return false;
            strcpy(buf, list);
            bool ok = false;
            char* token = buf;
            char* entry;
#ifdef WIN32
            char* sp = nullptr;
            entry = strtok_s(token, ",", &sp);
            while (entry) {
#else
            while ((entry = strsep(&token, ","))) {
#endif
                while (*entry == ' ' || *entry == '\t') entry++;
                char* e = entry + strlen(entry) - 1;
                while (e > entry && (*e == ' ' || *e == '\t')) *e-- = '\0';
                if (*entry) {
                    struct addrinfo hints {}, * res = nullptr;
                    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
                    if (!getaddrinfo(entry, nullptr, &hints, &res) && res) {
                        uint32_t r = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
                        freeaddrinfo(res);
                        if (r == ip_net) { ok = true; break; }
                    }
                    else if (res) freeaddrinfo(res);
                }
#ifdef WIN32
                entry = strtok_s(nullptr, ",", &sp);
#endif
            }
            free(buf);
            return ok;
            }

        // ─── Write worker ─────────────────────────────────────────────────────────────
        // Disk write blocking chạy ở đây, không block select() loop
        void WriteWorker::start()
        {
            thread = std::thread([this]() {
                for (;;) {
                    WriteJob job;
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cv.wait(lk, [this] { return stop.load() || !queue.empty(); });
                        if (stop.load() && queue.empty()) return;
                        job = std::move(queue.front());
                        queue.pop();
                    }

                    // Tạo dir nếu cần
                    size_t slash = job.fpath.rfind('/');
#ifdef WIN32
                    if (slash == std::string::npos) slash = job.fpath.rfind('\\');
#endif
                    if (slash != std::string::npos) make_dirs(job.fpath.substr(0, slash));

                    bool ok = write_atomic(job.fpath, job.data.data(), (uint32_t)job.data.size());

                    Result r;
                    r.sd = job.sd;
                    r.cmd = job.cmd;
                    r.result = ok ? D2FS_OK : D2FS_ERROR;
                    r.log_str = job.log_str + (ok ? " ok" : " FAILED");
                    {
                        std::lock_guard<std::mutex> lk(results_mtx);
                        results.push(std::move(r));
                    }
                }
                });
        }

        void WriteWorker::push(WriteJob job)
        {
            { std::lock_guard<std::mutex> lk(mtx); queue.push(std::move(job)); }
            cv.notify_one();
        }

        void WriteWorker::shutdown()
        {
            stop.store(true);
            cv.notify_all();
            if (thread.joinable()) thread.join();
        }

        bool WriteWorker::pop_result(Result & r)
        {
            std::lock_guard<std::mutex> lk(results_mtx);
            if (results.empty()) return false;
            r = std::move(results.front());
            results.pop();
            return true;
        }

        // ─── Build reply packet vào wbuf của connection ───────────────────────────────
        static void conn_queue_reply(D2FSConn & c, uint8_t orig_cmd, uint8_t result,
            const uint8_t * data = nullptr, uint32_t datalen = 0)
        {
            uint32_t total = (uint32_t)(sizeof(d2fs_reply_hdr) + datalen);
            if (c.wbuf_len - c.wbuf_sent + total > D2FS_WRITE_BUF_SIZE) {
                D2FS_LOG(eventlog_level_error, "[{}] wbuf full, dropping reply", c.peer.c_str());
                return;
            }
            // Shift unset data to front nếu cần
            if (c.wbuf_sent > 0 && c.wbuf_len > c.wbuf_sent) {
                memmove(c.wbuf.data(), c.wbuf.data() + c.wbuf_sent, c.wbuf_len - c.wbuf_sent);
                c.wbuf_len -= c.wbuf_sent;
                c.wbuf_sent = 0;
            }
            else if (c.wbuf_sent >= c.wbuf_len) {
                c.wbuf_len = c.wbuf_sent = 0;
            }

            uint8_t* p = c.wbuf.data() + c.wbuf_len;
            d2fs_reply_hdr* r = (d2fs_reply_hdr*)p;
            memset(r, 0, sizeof(*r));
            r->hdr.size = total;
            r->hdr.cmd = D2FS_CMD_REPLY;
            r->orig_cmd = orig_cmd;
            r->result = result;
            r->datalen = datalen;
            if (data && datalen)
                memcpy(p + sizeof(d2fs_reply_hdr), data, datalen);
            c.wbuf_len += total;
        }

        // ─── Xử lý packet hoàn chỉnh trong rbuf ──────────────────────────────────────
        // Trả về false nếu cần đóng connection
        static bool process_packet(D2FSConn & conn)
        {
            if (conn.rbuf_len < D2FS_HDR_SIZE) return true; // chưa đủ header

            d2fs_hdr* hdr = (d2fs_hdr*)conn.rbuf.data();

            if (hdr->size < D2FS_HDR_SIZE || hdr->size > D2FS_MAX_PKT) {
                D2FS_LOG(eventlog_level_error, "[{}] invalid packet size {}", conn.peer.c_str(), hdr->size);
                return false;
            }
            if (conn.rbuf_len < hdr->size) return true; // chưa đủ packet

            uint8_t  cmd = hdr->cmd;
            uint8_t* payload = conn.rbuf.data() + D2FS_HDR_SIZE;
            size_t   payload_size = hdr->size - D2FS_HDR_SIZE;

            std::string acc, chr;

            switch (cmd) {

            case D2FS_CMD_SAVE_CHAR:
            case D2FS_CMD_SAVE_STASH: {
                size_t nb = 0;
                if (!parse_names(payload, payload_size, acc, chr, &nb) || payload_size < nb + 4) {
                    conn_queue_reply(conn, cmd, D2FS_ERROR); break;
                }
                uint32_t datalen;
                memcpy(&datalen, payload + nb, 4);
                if (payload_size < nb + 4 + datalen) {
                    conn_queue_reply(conn, cmd, D2FS_ERROR); break;
                }

                // Đẩy vào write worker — không block select() loop
                WriteJob job;
                job.fpath = (cmd == D2FS_CMD_SAVE_CHAR)
                    ? char_path(acc, chr) : stash_path(acc, chr);
                job.data.assign(payload + nb + 4, payload + nb + 4 + datalen);
                job.cmd = cmd;
                job.sd = conn.sd;
                job.log_str = std::string("[") + conn.peer + "] "
                    + (cmd == D2FS_CMD_SAVE_CHAR ? "SaveChar" : "SaveStash")
                    + " " + acc + "/" + chr
                    + " datalen=" + std::to_string(datalen);
                D2FS_LOG(eventlog_level_info, "{}", job.log_str.c_str());
                s_writer.push(std::move(job));
                // Reply sẽ được gửi khi write worker xong
                break;
            }

            case D2FS_CMD_LOAD_CHAR:
            case D2FS_CMD_LOAD_STASH: {
                if (!parse_names(payload, payload_size, acc, chr, nullptr)) {
                    conn_queue_reply(conn, cmd, D2FS_ERROR); break;
                }
                std::string fpath = (cmd == D2FS_CMD_LOAD_CHAR)
                    ? char_path(acc, chr) : stash_path(acc, chr);
                // Read file — thường nhỏ và nhanh, đọc inline trong select() loop
                auto data = read_file(fpath);
                if (data.empty()) {
                    D2FS_LOG(eventlog_level_warn, "[{}] {} {}/{} NOT_FOUND",
                        conn.peer.c_str(), cmd == D2FS_CMD_LOAD_CHAR ? "LoadChar" : "LoadStash",
                        acc.c_str(), chr.c_str());
                    conn_queue_reply(conn, cmd, D2FS_NOT_FOUND);
                }
                else {
                    D2FS_LOG(eventlog_level_info, "[{}] {} {}/{} datalen={} ok",
                        conn.peer.c_str(), cmd == D2FS_CMD_LOAD_CHAR ? "LoadChar" : "LoadStash",
                        acc.c_str(), chr.c_str(), (unsigned)data.size());
                    conn_queue_reply(conn, cmd, D2FS_OK, data.data(), (uint32_t)data.size());
                }
                break;
            }

            default:
                D2FS_LOG(eventlog_level_warn, "[{}] unknown cmd=0x{:02X}", conn.peer.c_str(), cmd);
                conn_queue_reply(conn, cmd, D2FS_ERROR);
                break;
            }

            // Consume packet từ rbuf
            size_t pkt_size = hdr->size;
            if (conn.rbuf_len > pkt_size)
                memmove(conn.rbuf.data(), conn.rbuf.data() + pkt_size, conn.rbuf_len - pkt_size);
            conn.rbuf_len -= pkt_size;

            return true;
        }

        // ─── Firewall ─────────────────────────────────────────────────────────────────
#ifdef WIN32
        static void firewall_open(unsigned short port) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                "netsh advfirewall firewall add rule"
                " name=\"d2fs_%u\" protocol=TCP dir=in action=allow localport=%u > nul 2>&1",
                (unsigned)port, (unsigned)port);
            D2FS_LOG(eventlog_level_info, "firewall: {}", cmd); system(cmd);
        }
        static void firewall_close(unsigned short port) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                "netsh advfirewall firewall delete rule name=\"d2fs_%u\" > nul 2>&1",
                (unsigned)port);
            D2FS_LOG(eventlog_level_info, "firewall: {}", cmd); system(cmd);
        }
#else
        static bool cmd_exists(const char* bin) {
            char c[128]; snprintf(c, sizeof(c), "command -v %s > /dev/null 2>&1", bin);
            return system(c) == 0;
        }
        static void firewall_open(unsigned short port) {
            char cmd[512];
            if (cmd_exists("iptables")) {
                snprintf(cmd, sizeof(cmd),
                    "iptables -C INPUT -p tcp --dport %u -j ACCEPT 2>/dev/null"
                    " || iptables -I INPUT -p tcp --dport %u -j ACCEPT",
                    (unsigned)port, (unsigned)port);
                D2FS_LOG(eventlog_level_info, "firewall: {}", cmd); system(cmd); return;
            }
            if (cmd_exists("firewall-cmd")) {
                snprintf(cmd, sizeof(cmd),
                    "firewall-cmd --permanent --add-port=%u/tcp > /dev/null 2>&1"
                    " && firewall-cmd --reload > /dev/null 2>&1", (unsigned)port);
                D2FS_LOG(eventlog_level_info, "firewall: {}", cmd); system(cmd); return;
            }
            if (cmd_exists("ufw")) {
                snprintf(cmd, sizeof(cmd), "ufw allow %u/tcp > /dev/null 2>&1", (unsigned)port);
                D2FS_LOG(eventlog_level_info, "firewall: {}", cmd); system(cmd); return;
            }
            D2FS_LOG(eventlog_level_warn, "no firewall tool found, port {} may be blocked", port);
        }
        static void firewall_close(unsigned short port) {
            char cmd[512];
            if (cmd_exists("iptables")) {
                snprintf(cmd, sizeof(cmd),
                    "while iptables -C INPUT -p tcp --dport %u -j ACCEPT 2>/dev/null;"
                    " do iptables -D INPUT -p tcp --dport %u -j ACCEPT; done",
                    (unsigned)port, (unsigned)port);
                system(cmd); return;
            }
            if (cmd_exists("firewall-cmd")) {
                snprintf(cmd, sizeof(cmd),
                    "firewall-cmd --permanent --remove-port=%u/tcp > /dev/null 2>&1"
                    " && firewall-cmd --reload > /dev/null 2>&1", (unsigned)port);
                system(cmd); return;
            }
            if (cmd_exists("ufw")) {
                snprintf(cmd, sizeof(cmd), "ufw delete allow %u/tcp > /dev/null 2>&1", (unsigned)port);
                system(cmd); return;
            }
        }
#endif

        // ─── Server loop — giống dbs_server_loop() của d2dbs ─────────────────────────
        static void server_loop()
        {
            while (s_running.load()) {

                // Trước tiên poll write worker results, gửi reply cho connection tương ứng
                {
                    WriteWorker::Result wr;
                    while (s_writer.pop_result(wr)) {
                        D2FS_LOG(eventlog_level_info, "{}", wr.log_str.c_str());
                        for (auto& c : s_conns) {
                            if (c.sd == wr.sd) {
                                conn_queue_reply(c, wr.cmd, wr.result);
                                break;
                            }
                        }
                    }
                }

                // Build fd_set — giống d2dbs
                fd_set rset, wset, eset;
                FD_ZERO(&rset); FD_ZERO(&wset); FD_ZERO(&eset);
                FD_SET((unsigned)s_listen_sock, &rset);
                FD_SET((unsigned)s_listen_sock, &eset);
                int maxfd = (int)s_listen_sock;

                for (auto& c : s_conns) {
                    // Đọc nếu buffer chưa đầy
                    if (c.rbuf_len < D2FS_READ_BUF_SIZE)
                        FD_SET((unsigned)c.sd, &rset);
                    // Ghi nếu có data chờ
                    if (c.wbuf_len > c.wbuf_sent)
                        FD_SET((unsigned)c.sd, &wset);
                    FD_SET((unsigned)c.sd, &eset);
                    if ((int)c.sd > maxfd) maxfd = (int)c.sd;
                }

                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = D2FS_SELECT_TIMEOUT;

                int r = select(maxfd + 1, &rset, &wset, &eset, &tv);
                if (r < 0) continue;

                // Accept new connection
                if (FD_ISSET((unsigned)s_listen_sock, &rset)) {
                    struct sockaddr_in caddr {}; socklen_t clen = sizeof(caddr);
                    d2fs_sock_t cs = (d2fs_sock_t)accept((int)s_listen_sock,
                        (struct sockaddr*)&caddr, &clen);
                    if (!D2FS_SOCK_ERR(cs)) {
                        D2FS_NONBLOCK(cs);
                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
                        std::string peer = std::string(ip) + ":" + std::to_string(ntohs(caddr.sin_port));

                        if (!d2fs_verify_ip(caddr.sin_addr.s_addr)) {
                            D2FS_LOG(eventlog_level_error, "connection rejected: {}", peer.c_str());
                            D2FS_CLOSE(cs);
                        }
                        else {
                            s_conns.emplace_back(cs, caddr.sin_addr.s_addr, peer);
                            D2FS_LOG(eventlog_level_info, "client connected: {} (total={})",
                                peer.c_str(), (unsigned)s_conns.size());
                        }
                    }
                }

                // Process existing connections
                for (auto it = s_conns.begin(); it != s_conns.end(); ) {
                    D2FSConn& c = *it;
                    bool drop = false;

                    if (FD_ISSET((unsigned)c.sd, &eset)) {
                        drop = true;
                    }
                    else {
                        // Read data
                        if (FD_ISSET((unsigned)c.sd, &rset)) {
                            int n = recv(c.sd,
                                (char*)c.rbuf.data() + c.rbuf_len,
                                (int)(D2FS_READ_BUF_SIZE - c.rbuf_len), 0);
                            if (n <= 0 && !D2FS_WOULDBLOCK) {
                                drop = true;
                            }
                            else if (n > 0) {
                                c.rbuf_len += n;
                                c.last_active = std::time(nullptr);
                                // Xử lý tất cả packet hoàn chỉnh trong buffer
                                if (!process_packet(c)) drop = true;
                            }
                        }

                        // Write pending data
                        if (!drop && FD_ISSET((unsigned)c.sd, &wset) && c.wbuf_len > c.wbuf_sent) {
                            int n = send(c.sd,
                                (const char*)c.wbuf.data() + c.wbuf_sent,
                                (int)(c.wbuf_len - c.wbuf_sent), 0);
                            if (n < 0 && !D2FS_WOULDBLOCK) {
                                drop = true;
                            }
                            else if (n > 0) {
                                c.wbuf_sent += n;
                                if (c.wbuf_sent >= c.wbuf_len)
                                    c.wbuf_sent = c.wbuf_len = 0;
                            }
                        }
                    }

                    if (drop) {
                        D2FS_LOG(eventlog_level_info, "client disconnected: {} (total={})",
                            c.peer.c_str(), (unsigned)s_conns.size() - 1);
                        D2FS_CLOSE(c.sd);
                        it = s_conns.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }
        }

        static void server_thread_func(unsigned short port)
        {
            s_listen_sock = (d2fs_sock_t)socket(AF_INET, SOCK_STREAM, 0);
            if (D2FS_SOCK_ERR(s_listen_sock)) {
                D2FS_LOG(eventlog_level_error, "socket() failed"); return;
            }
            int yes = 1;
            setsockopt((int)s_listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

            struct sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);

            if (bind((int)s_listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                D2FS_LOG(eventlog_level_error, "bind() failed on port {}", port);
                D2FS_CLOSE(s_listen_sock); s_listen_sock = D2FS_INVALID_SOCK; return;
            }
            if (listen((int)s_listen_sock, SOMAXCONN) < 0) {
                D2FS_LOG(eventlog_level_error, "listen() failed");
                D2FS_CLOSE(s_listen_sock); s_listen_sock = D2FS_INVALID_SOCK; return;
            }
            D2FS_LOG(eventlog_level_info,
                "D2FileServer listening port={} datadir={}", port, s_datadir.c_str());

            server_loop();

            // Đóng tất cả connection
            for (auto& c : s_conns) D2FS_CLOSE(c.sd);
            s_conns.clear();
            D2FS_CLOSE(s_listen_sock);
            s_listen_sock = D2FS_INVALID_SOCK;
            D2FS_LOG(eventlog_level_info, "D2FileServer stopped");
        }

        // ─── Public API ───────────────────────────────────────────────────────────────
        void d2fs_server_start(unsigned short port, const char* datadir)
        {
            if (s_running.load()) return;
            s_datadir = datadir ? datadir : "./d2fsdata";
            s_port = port;

            make_dirs(s_datadir + "/charsave");
            make_dirs(s_datadir + "/charstash");

            firewall_open(port);
            s_writer.start();

            s_running.store(true);
            s_server_thread = std::thread(server_thread_func, port);

            D2FS_LOG(eventlog_level_info, "D2FileServer started port={} datadir={}",
                port, s_datadir.c_str());
        }

        void d2fs_server_stop(void)
        {
            if (!s_running.load()) return;
            s_running.store(false);
            if (s_server_thread.joinable()) s_server_thread.join();
            s_writer.shutdown();
            if (s_port) firewall_close(s_port);
            D2FS_LOG(eventlog_level_info, "D2FileServer stopped");
        }

        } // d2dbs
    } // pvpgn

///*
// * d2fs_server.cpp  —  D2FileServer tích hợp vào d2dbs
// *
// * Tính năng:
// *   - Thread pool cố định (D2FS_THREAD_POOL_SIZE workers) chịu nhiều kết nối đồng thời
// *   - Tự động mở port firewall khi start, đóng khi stop:
// *       Linux  : thử iptables → firewall-cmd → ufw theo thứ tự
// *       Windows: netsh advfirewall
// *   - Whitelist IP từ d2dbs_prefs_get_d2gs_list()
// *
// * Protocol: [uint32 size][uint8 cmd][uint8 pad[3]][payload...]
// *   CMD_SAVE_CHAR  0x01  [acc_len][chr_len][acc][chr][uint32 datalen][data]
// *   CMD_LOAD_CHAR  0x02  [acc_len][chr_len][acc][chr]
// *   CMD_SAVE_STASH 0x03  [acc_len][chr_len][acc][chr][uint32 datalen][data]
// *   CMD_LOAD_STASH 0x04  [acc_len][chr_len][acc][chr]
// *   CMD_REPLY      0x80  [orig_cmd][result][pad2][uint32 datalen][data...]
// *   result: 0=OK  1=NOT_FOUND  2=ERROR
// */
//
//#include "common/setup_before.h"
//#include "setup.h"
//#include "d2fs_server.h"
//
//#include <cstdio>
//#include <cstdlib>
//#include <cstring>
//#include <cstdint>
//#include <string>
//#include <vector>
//#include <queue>
//#include <thread>
//#include <mutex>
//#include <condition_variable>
//#include <atomic>
//
//#ifdef WIN32
//#  include <winsock2.h>
//#  include <ws2tcpip.h>
//#  include <direct.h>
//#  include <io.h>
//typedef SOCKET d2fs_sock_t;
//#  define D2FS_INVALID_SOCK  INVALID_SOCKET
//#  define D2FS_CLOSE(s)      closesocket(s)
//#  define d2fs_mkdir(p)      _mkdir(p)
//#  define d2fs_access(p)     _access(p, 0)
//#else
//#  include <sys/socket.h>
//#  include <netinet/in.h>
//#  include <arpa/inet.h>
//#  include <unistd.h>
//#  include <sys/stat.h>
//typedef int d2fs_sock_t;
//#  define D2FS_INVALID_SOCK  (-1)
//#  define D2FS_CLOSE(s)      close(s)
//#  define d2fs_mkdir(p)      mkdir(p, 0755)
//#  define d2fs_access(p)     access(p, F_OK)
//#endif
//
//#include "common/eventlog.h"
//#include "prefs.h"
//#include "common/setup_after.h"
//
//// ─── Cấu hình ─────────────────────────────────────────────────────────────────
//#define D2FS_THREAD_POOL_SIZE  16    // số worker thread cố định
//#define D2FS_QUEUE_MAX         256   // tối đa connection chờ trong queue
//
//// ─── Protocol ────────────────────────────────────────────────────────────────
//#define D2FS_HDR_SIZE    8u
//#define D2FS_MAX_ACC     31u
//#define D2FS_MAX_CHR     15u
//#define D2FS_MAX_PKT     (8u * 1024u * 1024u)
//
//#define D2FS_CMD_SAVE_CHAR   0x01
//#define D2FS_CMD_LOAD_CHAR   0x02
//#define D2FS_CMD_SAVE_STASH  0x03
//#define D2FS_CMD_LOAD_STASH  0x04
//#define D2FS_CMD_REPLY       0x80
//
//#define D2FS_OK         0
//#define D2FS_NOT_FOUND  1
//#define D2FS_ERROR      2
//
//#pragma pack(push, 1)
//struct d2fs_hdr {
//    uint32_t size;
//    uint8_t  cmd;
//    uint8_t  pad[3];
//};
//struct d2fs_reply_hdr {
//    d2fs_hdr hdr;
//    uint8_t  orig_cmd;
//    uint8_t  result;
//    uint8_t  pad2[2];
//    uint32_t datalen;
//};
//#pragma pack(pop)
//
//namespace pvpgn {
//    namespace d2dbs {
//
//        // ─── Thread pool ──────────────────────────────────────────────────────────────
//        struct ClientJob {
//            d2fs_sock_t sock;
//            uint32_t    ip_net;   // network byte order
//            std::string peer;
//        };
//
//        struct ThreadPool {
//            std::vector<std::thread>  workers;
//            std::queue<ClientJob>     queue;
//            std::mutex                mtx;
//            std::condition_variable   cv;
//            std::atomic<bool>         stop{ false };
//
//            void start(int n);
//            void enqueue(ClientJob job);
//            void shutdown();
//        };
//
//        // ─── State ────────────────────────────────────────────────────────────────────
//        static std::atomic<bool> s_running{ false };
//        static std::thread       s_accept_thread;
//        static d2fs_sock_t       s_listen_sock = D2FS_INVALID_SOCK;
//        static std::string       s_datadir;
//        static unsigned short    s_port = 0;
//        static ThreadPool        s_pool;
//
//        // ─── File helpers ─────────────────────────────────────────────────────────────
//        static void make_dirs(const std::string& path)
//        {
//            for (size_t i = 1; i < path.size(); ++i) {
//                if (path[i] == '/' || path[i] == '\\') {
//                    std::string sub = path.substr(0, i);
//                    if (d2fs_access(sub.c_str()) != 0) d2fs_mkdir(sub.c_str());
//                }
//            }
//            if (d2fs_access(path.c_str()) != 0) d2fs_mkdir(path.c_str());
//        }
//
//        static std::string sanitize(const char* s, size_t len)
//        {
//            std::string out(s, len);
//            for (auto& c : out) {
//                c = (char)tolower((unsigned char)c);
//                if (!isalnum((unsigned char)c) && c != '_' && c != '-') c = '_';
//            }
//            return out;
//        }
//
//        static std::string char_path(const std::string& acc, const std::string& chr) {
//            return s_datadir + "/charsave/" + acc + "/" + chr + ".d2s";
//        }
//        static std::string stash_path(const std::string& acc, const std::string& chr) {
//            return s_datadir + "/charstash/" + acc + "/" + chr + ".stash";
//        }
//
//        static bool write_atomic(const std::string& path, const uint8_t* data, uint32_t len)
//        {
//            std::string tmp = path + ".tmp";
//            FILE* f = fopen(tmp.c_str(), "wb");
//            if (!f) return false;
//            bool ok = (fwrite(data, 1, len, f) == len);
//            fclose(f);
//            if (!ok) { remove(tmp.c_str()); return false; }
//#ifdef WIN32
//            remove(path.c_str());
//#endif
//            return rename(tmp.c_str(), path.c_str()) == 0;
//        }
//
//        static std::vector<uint8_t> read_file(const std::string& path)
//        {
//            FILE* f = fopen(path.c_str(), "rb");
//            if (!f) return {};
//            fseek(f, 0, SEEK_END);
//            long sz = ftell(f); rewind(f);
//            if (sz <= 0) { fclose(f); return {}; }
//            std::vector<uint8_t> buf((size_t)sz);
//            bool ok = (fread(buf.data(), 1, (size_t)sz, f) == (size_t)sz);
//            fclose(f);
//            return ok ? buf : std::vector<uint8_t>{};
//        }
//
//        // ─── IP whitelist ─────────────────────────────────────────────────────────────
//        static bool d2fs_verify_ip(uint32_t client_ip_net)
//        {
//            const char* list = d2dbs_prefs_get_d2gs_list();
//            if (!list || list[0] == '\0') return false;
//
//            char* buf = (char*)malloc(strlen(list) + 1);
//            if (!buf) return false;
//            strcpy(buf, list);
//
//            bool allowed = false;
//            char* token = buf;
//            char* entry;
//
//#ifdef WIN32
//            char* saveptr = nullptr;
//            entry = strtok_s(token, ",", &saveptr);
//            while (entry) {
//#else
//            while ((entry = strsep(&token, ","))) {
//#endif
//                while (*entry == ' ' || *entry == '\t') entry++;
//                char* end = entry + strlen(entry) - 1;
//                while (end > entry && (*end == ' ' || *end == '\t')) { *end = '\0'; end--; }
//
//                if (entry[0] != '\0') {
//                    struct addrinfo hints {}, * res = nullptr;
//                    hints.ai_family = AF_INET;
//                    hints.ai_socktype = SOCK_STREAM;
//                    if (getaddrinfo(entry, nullptr, &hints, &res) == 0 && res) {
//                        uint32_t resolved = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
//                        freeaddrinfo(res);
//                        if (resolved == client_ip_net) { allowed = true; break; }
//                    }
//                    else if (res) {
//                        freeaddrinfo(res);
//                    }
//                }
//#ifdef WIN32
//                entry = strtok_s(nullptr, ",", &saveptr);
//#endif
//            }
//
//            free(buf);
//            return allowed;
//            }
//
//        // ─── Firewall ─────────────────────────────────────────────────────────────────
//        static void fw_run(const char* cmd)
//        {
//            eventlog(eventlog_level_info, "d2fs", "firewall: {}", cmd);
//            int r = system(cmd);
//            if (r != 0)
//                eventlog(eventlog_level_warn, "d2fs", "firewall command returned {}", r);
//        }
//
//#ifdef WIN32
//
//        static void firewall_open(unsigned short port)
//        {
//            char cmd[512];
//            snprintf(cmd, sizeof(cmd),
//                "netsh advfirewall firewall add rule"
//                " name=\"d2fs_%u\" protocol=TCP dir=in action=allow localport=%u > nul 2>&1",
//                (unsigned)port, (unsigned)port);
//            fw_run(cmd);
//        }
//
//        static void firewall_close(unsigned short port)
//        {
//            char cmd[256];
//            snprintf(cmd, sizeof(cmd),
//                "netsh advfirewall firewall delete rule name=\"d2fs_%u\" > nul 2>&1",
//                (unsigned)port);
//            fw_run(cmd);
//        }
//
//#else // Linux
//
//        static bool cmd_exists(const char* bin)
//        {
//            char check[128];
//            snprintf(check, sizeof(check), "command -v %s > /dev/null 2>&1", bin);
//            return system(check) == 0;
//        }
//
//        static void firewall_open(unsigned short port)
//        {
//            char cmd[512];
//
//            if (cmd_exists("iptables")) {
//                // Thêm rule nếu chưa có (tránh duplicate)
//                snprintf(cmd, sizeof(cmd),
//                    "iptables -C INPUT -p tcp --dport %u -j ACCEPT 2>/dev/null"
//                    " || iptables -I INPUT -p tcp --dport %u -j ACCEPT",
//                    (unsigned)port, (unsigned)port);
//                fw_run(cmd); return;
//            }
//            if (cmd_exists("firewall-cmd")) {
//                snprintf(cmd, sizeof(cmd),
//                    "firewall-cmd --permanent --add-port=%u/tcp > /dev/null 2>&1"
//                    " && firewall-cmd --reload > /dev/null 2>&1",
//                    (unsigned)port);
//                fw_run(cmd); return;
//            }
//            if (cmd_exists("ufw")) {
//                snprintf(cmd, sizeof(cmd),
//                    "ufw allow %u/tcp > /dev/null 2>&1", (unsigned)port);
//                fw_run(cmd); return;
//            }
//            eventlog(eventlog_level_warn, "d2fs",
//                "no firewall tool found (iptables/firewall-cmd/ufw), port {} may be blocked", port);
//        }
//
//        static void firewall_close(unsigned short port)
//        {
//            char cmd[512];
//
//            if (cmd_exists("iptables")) {
//                // Xóa tất cả rule trùng
//                snprintf(cmd, sizeof(cmd),
//                    "while iptables -C INPUT -p tcp --dport %u -j ACCEPT 2>/dev/null;"
//                    " do iptables -D INPUT -p tcp --dport %u -j ACCEPT; done",
//                    (unsigned)port, (unsigned)port);
//                fw_run(cmd); return;
//            }
//            if (cmd_exists("firewall-cmd")) {
//                snprintf(cmd, sizeof(cmd),
//                    "firewall-cmd --permanent --remove-port=%u/tcp > /dev/null 2>&1"
//                    " && firewall-cmd --reload > /dev/null 2>&1",
//                    (unsigned)port);
//                fw_run(cmd); return;
//            }
//            if (cmd_exists("ufw")) {
//                snprintf(cmd, sizeof(cmd),
//                    "ufw delete allow %u/tcp > /dev/null 2>&1", (unsigned)port);
//                fw_run(cmd); return;
//            }
//        }
//
//#endif // Linux
//
//        // ─── TCP helpers ──────────────────────────────────────────────────────────────
//        static bool tcp_recv(d2fs_sock_t s, void* buf, size_t len)
//        {
//            char* p = (char*)buf; size_t got = 0;
//            while (got < len) {
//                int r = recv(s, p + got, (int)(len - got), 0);
//                if (r <= 0) return false;
//                got += r;
//            }
//            return true;
//        }
//
//        static bool tcp_send(d2fs_sock_t s, const void* buf, size_t len)
//        {
//            const char* p = (const char*)buf; size_t sent = 0;
//            while (sent < len) {
//                int r = send(s, p + sent, (int)(len - sent), 0);
//                if (r <= 0) return false;
//                sent += r;
//            }
//            return true;
//        }
//
//        static bool send_reply(d2fs_sock_t s, uint8_t orig_cmd, uint8_t result,
//            const uint8_t * data = nullptr, uint32_t datalen = 0)
//        {
//            uint32_t total = (uint32_t)(sizeof(d2fs_reply_hdr) + datalen);
//            std::vector<uint8_t> pkt(total, 0);
//            d2fs_reply_hdr* r = (d2fs_reply_hdr*)pkt.data();
//            r->hdr.size = total; r->hdr.cmd = D2FS_CMD_REPLY;
//            r->orig_cmd = orig_cmd; r->result = result; r->datalen = datalen;
//            if (data && datalen)
//                memcpy(pkt.data() + sizeof(d2fs_reply_hdr), data, datalen);
//            return tcp_send(s, pkt.data(), pkt.size());
//        }
//
//        static bool parse_names(const uint8_t * p, size_t ps,
//            std::string & acc, std::string & chr,
//            size_t * consumed = nullptr)
//        {
//            if (ps < 2) return false;
//            uint8_t al = p[0], cl = p[1];
//            if (al == 0 || al > D2FS_MAX_ACC) return false;
//            if (cl == 0 || cl > D2FS_MAX_CHR) return false;
//            if (ps < (size_t)(2 + al + cl)) return false;
//            acc = sanitize((const char*)p + 2, al);
//            chr = sanitize((const char*)p + 2 + al, cl);
//            if (consumed) *consumed = 2 + al + cl;
//            return true;
//        }
//
//        // ─── Per-client handler (chạy trong worker thread) ───────────────────────────
//        static void handle_client(const ClientJob & job)
//        {
//            d2fs_sock_t s = job.sock;
//            const std::string& peer = job.peer;
//
//            if (!d2fs_verify_ip(job.ip_net)) {
//                eventlog(eventlog_level_error, "d2fs",
//                    "connection rejected from unknown ip: {}", peer.c_str());
//                D2FS_CLOSE(s);
//                return;
//            }
//
//            eventlog(eventlog_level_info, "d2fs", "client connected: {}", peer.c_str());
//
//            for (;;) {
//                d2fs_hdr hdr;
//                if (!tcp_recv(s, &hdr, sizeof(hdr))) break;
//
//                if (hdr.size < D2FS_HDR_SIZE || hdr.size > D2FS_MAX_PKT) {
//                    eventlog(eventlog_level_error, "d2fs",
//                        "[{}] invalid packet size {}", peer.c_str(), hdr.size);
//                    break;
//                }
//
//                uint32_t payload_size = hdr.size - D2FS_HDR_SIZE;
//                std::vector<uint8_t> payload(payload_size);
//                if (payload_size > 0 && !tcp_recv(s, payload.data(), payload_size)) break;
//
//                const uint8_t* p = payload.data();
//                size_t ps = payload_size;
//                std::string acc, chr;
//
//                switch (hdr.cmd) {
//
//                case D2FS_CMD_SAVE_CHAR:
//                case D2FS_CMD_SAVE_STASH: {
//                    size_t name_bytes = 0;
//                    if (!parse_names(p, ps, acc, chr, &name_bytes) || ps < name_bytes + 4) {
//                        send_reply(s, hdr.cmd, D2FS_ERROR); break;
//                    }
//                    uint32_t datalen;
//                    memcpy(&datalen, p + name_bytes, 4);
//                    if (ps < name_bytes + 4 + datalen) {
//                        send_reply(s, hdr.cmd, D2FS_ERROR); break;
//                    }
//                    const uint8_t* data = p + name_bytes + 4;
//                    std::string fpath = (hdr.cmd == D2FS_CMD_SAVE_CHAR)
//                        ? char_path(acc, chr) : stash_path(acc, chr);
//                    size_t slash = fpath.rfind('/');
//#ifdef WIN32
//                    if (slash == std::string::npos) slash = fpath.rfind('\\');
//#endif
//                    if (slash != std::string::npos) make_dirs(fpath.substr(0, slash));
//
//                    bool ok = write_atomic(fpath, data, datalen);
//                    eventlog(ok ? eventlog_level_info : eventlog_level_error, "d2fs",
//                        "[{}] {} {}/{} datalen={} {}",
//                        peer.c_str(),
//                        hdr.cmd == D2FS_CMD_SAVE_CHAR ? "SaveChar" : "SaveStash",
//                        acc.c_str(), chr.c_str(), datalen, ok ? "ok" : "FAILED");
//                    send_reply(s, hdr.cmd, ok ? D2FS_OK : D2FS_ERROR);
//                    break;
//                }
//
//                case D2FS_CMD_LOAD_CHAR:
//                case D2FS_CMD_LOAD_STASH: {
//                    if (!parse_names(p, ps, acc, chr, nullptr)) {
//                        send_reply(s, hdr.cmd, D2FS_ERROR); break;
//                    }
//                    std::string fpath = (hdr.cmd == D2FS_CMD_LOAD_CHAR)
//                        ? char_path(acc, chr) : stash_path(acc, chr);
//                    auto data = read_file(fpath);
//                    if (data.empty()) {
//                        eventlog(eventlog_level_warn, "d2fs", "[{}] {} {}/{} NOT_FOUND",
//                            peer.c_str(),
//                            hdr.cmd == D2FS_CMD_LOAD_CHAR ? "LoadChar" : "LoadStash",
//                            acc.c_str(), chr.c_str());
//                        send_reply(s, hdr.cmd, D2FS_NOT_FOUND);
//                    }
//                    else {
//                        eventlog(eventlog_level_info, "d2fs", "[{}] {} {}/{} datalen={} ok",
//                            peer.c_str(),
//                            hdr.cmd == D2FS_CMD_LOAD_CHAR ? "LoadChar" : "LoadStash",
//                            acc.c_str(), chr.c_str(), (unsigned)data.size());
//                        send_reply(s, hdr.cmd, D2FS_OK, data.data(), (uint32_t)data.size());
//                    }
//                    break;
//                }
//
//                default:
//                    eventlog(eventlog_level_warn, "d2fs",
//                        "[{}] unknown cmd=0x{:02X}", peer.c_str(), hdr.cmd);
//                    send_reply(s, hdr.cmd, D2FS_ERROR);
//                    break;
//                }
//            }
//
//            D2FS_CLOSE(s);
//            eventlog(eventlog_level_info, "d2fs", "client disconnected: {}", peer.c_str());
//        }
//
//        // ─── Thread pool implementation ───────────────────────────────────────────────
//        void ThreadPool::start(int n)
//        {
//            for (int i = 0; i < n; ++i) {
//                workers.emplace_back([this]() {
//                    for (;;) {
//                        ClientJob job;
//                        {
//                            std::unique_lock<std::mutex> lk(mtx);
//                            cv.wait(lk, [this] { return stop.load() || !queue.empty(); });
//                            if (stop.load() && queue.empty()) return;
//                            job = std::move(queue.front());
//                            queue.pop();
//                        }
//                        handle_client(job);
//                    }
//                    });
//            }
//            eventlog(eventlog_level_info, "d2fs",
//                "thread pool started: {} workers, queue max={}", n, D2FS_QUEUE_MAX);
//        }
//
//        void ThreadPool::enqueue(ClientJob job)
//        {
//            {
//                std::lock_guard<std::mutex> lk(mtx);
//                if ((int)queue.size() >= D2FS_QUEUE_MAX) {
//                    eventlog(eventlog_level_warn, "d2fs",
//                        "thread pool queue full ({}), dropping: {}",
//                        D2FS_QUEUE_MAX, job.peer.c_str());
//                    D2FS_CLOSE(job.sock);
//                    return;
//                }
//                queue.push(std::move(job));
//            }
//            cv.notify_one();
//        }
//
//        void ThreadPool::shutdown()
//        {
//            stop.store(true);
//            cv.notify_all();
//            for (auto& t : workers)
//                if (t.joinable()) t.join();
//            workers.clear();
//            // Đóng socket còn trong queue chưa xử lý
//            std::lock_guard<std::mutex> lk(mtx);
//            while (!queue.empty()) {
//                D2FS_CLOSE(queue.front().sock);
//                queue.pop();
//            }
//        }
//
//        // ─── Accept thread ────────────────────────────────────────────────────────────
//        static void accept_thread_func(unsigned short port)
//        {
//            s_listen_sock = (d2fs_sock_t)socket(AF_INET, SOCK_STREAM, 0);
//            if (s_listen_sock == D2FS_INVALID_SOCK) {
//                eventlog(eventlog_level_error, "d2fs", "socket() failed");
//                return;
//            }
//
//            int yes = 1;
//            setsockopt((int)s_listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
//
//            sockaddr_in addr{};
//            addr.sin_family = AF_INET;
//            addr.sin_addr.s_addr = INADDR_ANY;
//            addr.sin_port = htons(port);
//
//            if (bind((int)s_listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
//                eventlog(eventlog_level_error, "d2fs", "bind() failed on port {}", port);
//                D2FS_CLOSE(s_listen_sock); s_listen_sock = D2FS_INVALID_SOCK; return;
//            }
//            if (listen((int)s_listen_sock, D2FS_QUEUE_MAX) < 0) {
//                eventlog(eventlog_level_error, "d2fs", "listen() failed");
//                D2FS_CLOSE(s_listen_sock); s_listen_sock = D2FS_INVALID_SOCK; return;
//            }
//
//            eventlog(eventlog_level_info, "d2fs",
//                "D2FileServer listening port={} workers={} queue={}",
//                port, D2FS_THREAD_POOL_SIZE, D2FS_QUEUE_MAX);
//
//            while (s_running.load()) {
//                fd_set rset; FD_ZERO(&rset); FD_SET((unsigned)s_listen_sock, &rset);
//                struct timeval tv { 1, 0 };
//                if (select((int)s_listen_sock + 1, &rset, nullptr, nullptr, &tv) <= 0)
//                    continue;
//
//                sockaddr_in caddr{}; socklen_t clen = sizeof(caddr);
//                d2fs_sock_t cs = (d2fs_sock_t)accept((int)s_listen_sock, (sockaddr*)&caddr, &clen);
//                if (cs == D2FS_INVALID_SOCK) continue;
//
//                char ip[INET_ADDRSTRLEN];
//                inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
//                std::string peer = std::string(ip) + ":" + std::to_string(ntohs(caddr.sin_port));
//
//                // Đẩy vào pool, không tạo thread mới mỗi connection
//                s_pool.enqueue({ cs, caddr.sin_addr.s_addr, peer });
//            }
//
//            D2FS_CLOSE(s_listen_sock);
//            s_listen_sock = D2FS_INVALID_SOCK;
//            eventlog(eventlog_level_info, "d2fs", "D2FileServer accept loop stopped");
//        }
//
//        // ─── Public API ───────────────────────────────────────────────────────────────
//        void d2fs_server_start(unsigned short port, const char* datadir)
//        {
//            if (s_running.load()) return;
//
//            s_datadir = datadir ? datadir : "./d2fsdata";
//            s_port = port;
//
//            make_dirs(s_datadir + "/charsave");
//            make_dirs(s_datadir + "/charstash");
//
//            // Mở port firewall trước khi bind
//            firewall_open(port);
//
//            // Khởi động thread pool
//            s_pool.start(D2FS_THREAD_POOL_SIZE);
//
//            // Khởi động accept thread
//            s_running.store(true);
//            s_accept_thread = std::thread(accept_thread_func, port);
//
//            eventlog(eventlog_level_info, "d2fs",
//                "D2FileServer started port={} datadir={}", port, s_datadir.c_str());
//        }
//
//        void d2fs_server_stop(void)
//        {
//            if (!s_running.load()) return;
//
//            // Dừng accept loop
//            s_running.store(false);
//            if (s_accept_thread.joinable())
//                s_accept_thread.join();
//
//            // Dừng pool, đợi jobs đang chạy xong
//            s_pool.shutdown();
//
//            // Đóng port firewall
//            if (s_port) firewall_close(s_port);
//
//            eventlog(eventlog_level_info, "d2fs", "D2FileServer stopped");
//        }
//
//        } // d2dbs
//    } // pvpgn