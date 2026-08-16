# PvPGN Server Mod — Những gì khác so với PvPGN gốc

Tài liệu này liệt kê toàn bộ phần tự thêm vào so với PvPGN (bnetd) gốc, dựa
trên rà soát trực tiếp source code hiện tại. Mọi thứ dưới đây **không tồn
tại** trong PvPGN upstream.

## 1. Packet BNCS tùy chỉnh (dải `0xF0FF` – `0xF6FF`)

PvPGN gốc dùng các mã packet chuẩn (`0x00`–`0xEF` phần lớn). Bản mod này
chiếm dụng dải mã cao `0xF0`–`0xF6` (dạng little-endian `0xfXff`) để thêm 7
packet BNCS riêng, không đụng vào packet gốc nào.

| Mã | Tên | Chiều | Định nghĩa tại |
|---|---|---|---|
| `0xf0ff` | `SERVER_GAME_HOST_INFO` | S → C | `common/anongame_protocol.h` |
| `0xf1ff` | `CLIENT_CUSTOM_WAR3_VERSION` | C → S | `common/bnet_protocol.h` |
| `0xf2ff` | `CLIENT_REQUEST_GAME_LIST` | C → S | `common/bnet_protocol.h` |
| `0xf3ff` | `CLIENT_REQUEST_RANK_UPDATE` | C → S | `common/bnet_protocol.h` |
| `0xf4ff` | `SERVER_GAME_STATE_UPDATE` | S → C | `common/anongame_protocol.h` |
| `0xf5ff` | `CLIENT_CHANGE_PASSWORD` | C → S | `common/bnet_protocol.h` |
| `0xf6ff` | `SERVER_CHANGE_PASSWORD_RESULT` | S → C | `common/anongame_protocol.h` |

Toàn bộ handler nằm trong `bnetd/handle_bnet.cpp`, đăng ký vào bảng
`bnet_htable_log[]` (chỉ xử lý khi connection đã đăng nhập).

---

## 2. `SERVER_GAME_HOST_INFO` / `CLIENT_REQUEST_GAME_LIST` — Custom game listing

Thay vì client phải dùng cơ chế channel/game-list gốc của Battle.net (vốn có
giới hạn hiển thị và độ trễ), server gửi thẳng toàn bộ danh sách game đang mở
qua 1 packet duy nhất, chứa đầy đủ:

- `game_id`, `host_name`, `owner_host_name`, `game_name`, `game_password`
- `map_name`, địa chỉ IP:Port của host và owner
- `version`, số người chơi hiện tại/tối đa, trạng thái game
- `elapsed_time` (thời gian đã chạy), `stat_string` (thông tin thêm)

**Nơi triển khai:**
- `bnetd/game.cpp`: `game_send_list_to_connection()`, `game_send_single_to_connection()`, `game_broadcast_list_to_all_connections()`
- `bnetd/handle_bnet.cpp`: `_client_request_game_list()` — client chủ động yêu cầu danh sách qua `CLIENT_REQUEST_GAME_LIST`

## 3. `SERVER_GAME_STATE_UPDATE` — Cập nhật realtime

Khi trạng thái 1 game thay đổi (số người chơi, elapsed time, status), server
**broadcast** 1 packet nhỏ (`t_game_state_update_data`: chỉ `game_id`,
`current_players`, `game_status`, `elapsed_time`) tới **toàn bộ connection**
đang kết nối, thay vì phải gửi lại toàn bộ danh sách game.

**Nơi triển khai:** `bnetd/game.cpp` → `game_broadcast_state_update()`

## 4. `CLIENT_CUSTOM_WAR3_VERSION` — Ép version check theo thời gian thực

Cho phép client gửi lại `version` / `gameversion` / `checksum` **sau khi đã
đăng nhập** (không chỉ lúc login), để server re-lookup `VersionCheck` và cập
nhật `version_tag` — hữu ích khi client tự vá/đổi phiên bản War3 giữa
session mà không cần reconnect lại BNET.

**Nơi triển khai:** `bnetd/handle_bnet.cpp` → `_client_request_custom_war3_version()`

## 5. `CLIENT_REQUEST_RANK_UPDATE` — Hệ thống rank/level riêng

Toàn bộ hệ thống rank **không dùng bảng MySQL/`account_get_numattr` gốc**,
mà lưu trực tiếp ra file JSON theo từng user tại `var\rankdata\<username>.json`.

**Cách tính EXP:**
- Chỉ tính khi thời gian game ≥ 600 giây (10 phút), game ngắn hơn bị bỏ qua.
- `EXP = 100 + (số phút chơi × 10)`, tối đa 5000 EXP/trận.

**Hệ thống level (100 cấp, 10 rank tier):**
- Công thức chi phí: `cost(level) = 15 × level²` — càng lên cao càng đắt.
- Tổng EXP cần để đạt Lv.100: **5,075,235**.

| Tier | Level | Tên |
|---|---|---|
| 1 | 1–10 | Peasant |
| 2 | 11–20 | Footman |
| 3 | 21–30 | Knight |
| 4 | 31–40 | Paladin |
| 5 | 41–50 | Champion |
| 6 | 51–60 | Warlord |
| 7 | 61–70 | Archmage |
| 8 | 71–80 | General |
| 9 | 81–90 | Overlord |
| 10 | 91–100 | Legend |

Packet nhận vào chứa: thời lượng game, số người chơi (giới hạn 1–24), và
danh sách username. Với mỗi username, server đọc/tạo file rank JSON, cộng
EXP, tính lại level/tier, rồi ghi lại file.

**Nơi triển khai:** `bnetd/handle_bnet.cpp`
- Struct `RankData`, hàm `rank_ensure_dir()`, `rank_read_file()`, `rank_write_file()`
- Handler chính: `_client_rank_update()`

## 6. `CLIENT_CHANGE_PASSWORD` / `SERVER_CHANGE_PASSWORD_RESULT` — Đổi mật khẩu qua packet

Tương đương lệnh chat `/chpass <username> <newpass>`, nhưng qua packet BNCS
thay vì gõ lệnh chat thủ công — dùng cho client (GProxy) có nút bấm đổi mật
khẩu trực tiếp trong giao diện.

**Định dạng packet:**
- `CLIENT_CHANGE_PASSWORD` (C→S): `[username: chuỗi kết thúc bằng NUL][mật khẩu mới: chuỗi kết thúc bằng NUL]`
- `SERVER_CHANGE_PASSWORD_RESULT` (S→C): `[1 byte result_code][message: chuỗi kết thúc bằng NUL]`
  - `result_code = 0`: thành công
  - `1`: không có quyền đổi mật khẩu cho tài khoản khác
  - `2`: tài khoản không tồn tại
  - `3`: mật khẩu rỗng
  - `4`: mật khẩu quá dài (> `MAX_USERPASS_LEN` = 12 ký tự)
  - `5`: lỗi ghi vào account (`account_set_pass` thất bại)

**Quan trọng:** Mật khẩu mới gửi ở dạng **plaintext** qua kết nối BNCS —
KHÔNG hash sẵn phía client. Server tự `bnet_hash()` mật khẩu (sau khi chuyển
thường) trước khi lưu, **giống hệt** cách `_handle_chpass_command` (lệnh chat
`/chpass` gốc) hoạt động — không phải lỗ hổng bảo mật, đây là hành vi đúng
theo thiết kế gốc của PvPGN cho tính năng này. Không cần mật khẩu cũ: quyền
hạn được xác thực qua chính connection đang đăng nhập, đúng y hệt logic lệnh
`/chpass` — tự đổi cho mình luôn được phép (trừ khi tài khoản bị khoá đổi mật
khẩu qua `account_get_auth_changepass`), đổi cho **người khác** cần quyền
group `/admin-chpass`.

**Nơi triển khai:** `bnetd/handle_bnet.cpp`
- `_client_change_password()` — handler chính, dùng lại nguyên logic quyền
  hạn và cách hash từ `_handle_chpass_command` (`bnetd/command.cpp`)
- `_send_change_password_result()` — helper gửi phản hồi

---

## Tổng kết thay đổi theo file

| File | Thay đổi |
|---|---|
| `common/bnet_protocol.h` | Thêm struct + define cho `CLIENT_CUSTOM_WAR3_VERSION`, `CLIENT_REQUEST_GAME_LIST`, `CLIENT_REQUEST_RANK_UPDATE`, `CLIENT_CHANGE_PASSWORD` |
| `common/anongame_protocol.h` | Thêm struct + define cho `SERVER_GAME_HOST_INFO`, `SERVER_GAME_STATE_UPDATE`, `SERVER_CHANGE_PASSWORD_RESULT` |
| `bnetd/game.cpp` | `game_send_list_to_connection()`, `game_send_single_to_connection()`, `game_broadcast_state_update()`, `game_broadcast_list_to_all_connections()` |
| `bnetd/handle_bnet.cpp` | Toàn bộ handler cho 7 packet trên, hệ thống rank JSON (`RankData` + I/O file) |

Client tương ứng (GProxy) implement các packet C→S ở `bnetprotocol.cpp` /
`bnet.cpp`, xem thêm trong source GProxy.
