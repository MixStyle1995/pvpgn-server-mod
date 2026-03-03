#ifndef INCLUDED_D2FS_SERVER_H
#define INCLUDED_D2FS_SERVER_H

/*
 * d2fs_server.h  —  D2FileServer tích hợp vào d2dbs
 *
 * Gọi d2fs_server_start() từ dbs_server_main() trước khi vào loop.
 * Gọi d2fs_server_stop()  từ dbs_on_exit() khi shutdown.
 *
 * Server chạy trên std::thread riêng, dùng cùng port với d2dbs
 * nhưng trên port+1 (ví dụ d2dbs=6114 thì d2fs=6115).
 * Hoặc set d2fs_port riêng trong d2dbs.conf:
 *   d2fs_port = 6115
 */

namespace pvpgn {
namespace d2dbs {

    /* Khởi động D2FileServer thread.
     * port     = port để listen (thường DEFAULT_LISTEN_PORT + 1)
     * datadir  = thư mục gốc lưu file (lấy từ d2dbs_prefs)
     */
    void d2fs_server_start(unsigned short port, const char* datadir);

    /* Dừng D2FileServer thread (gọi khi d2dbs shutdown). */
    void d2fs_server_stop(void);

} // d2dbs
} // pvpgn

#endif
