# Sử dụng School Node

## Firmware

Mở `firmware/School_Node/School_Node.ino` bằng Arduino IDE. Dùng Arduino-ESP32 3.3.2 và ArduinoJson 7.3.2; ESP_I2S, LittleFS, Preferences và Wire thuộc core. Chọn flash/PSRAM đúng module, không bật OPI PSRAM nếu phần cứng không hỗ trợ.

Copy `secrets.example.h` thành `secrets.h` cùng thư mục. `TB_HOST` và `TB_TOKEN` để trống trong cấu hình mẫu; chỉ điền ở máy cá nhân. `secrets.h` đã được .gitignore loại trừ. Token rỗng khiến tác vụ ThingsBoard không được kích hoạt.

## TLS và kết nối

`request` (trong modem_http.h) chỉ chọn context TLS khi tham số `googleTLS` được bật ở yêu cầu TTS. Các lời gọi ThingsBoard không bật tham số này; URL HTTPS bị từ chối với mã -12 trước khi gửi yêu cầu HTTP. Không có tự động hạ từ HTTPS xuống HTTP.

Cấu hình TTS trong mã yêu cầu xác thực server, kiểm tra thời gian và SNI, dùng CA riêng cho TTS. Đây là quan sát mã nguồn, chưa xác minh bắt tay TLS trên modem thật. Không dùng CA/context của TTS để giả định ThingsBoard đã được xác thực.

Muốn triển khai ThingsBoard qua HTTPS cần bổ sung context TLS và CA phù hợp server, kiểm tra thời gian modem và thử các trường hợp chứng chỉ sai/hết hạn trên modem thật. Chỉ sửa TB_HOST sang HTTPS không hoàn thành bước này. HTTP truyền token và nội dung không mã hóa; không dùng cho triển khai thực tế cần bảo mật.

## Chân kết nối

| Tín hiệu | GPIO ESP32-S3 |
|---|---:|
| Modem TX / RX | 4 / 5 |
| I2S MCLK / BCLK / LRCK / DOUT | 15 / 16 / 17 / 18 |
| SHT31 SDA / SCL | 8 / 9 |
| Cảm biến lửa digital | 7 |

Firmware không điều khiển RST/PWRKEY modem. Kiểm tra đấu dây và cấp nguồn theo phần cứng thực tế.

## ThingsBoard

Import `thingsboard/dashboard.json`. Sửa entity alias `demo-device`, `settings.controlDeviceId` của widget văn bản/MP3 và `targetDevice.deviceId` của widget âm lượng. UUID toàn số 0 là giá trị mẫu. Các JSON trong `thingsboard/widgets` là widget instance để import vào dashboard.

Mở bảng điều khiển văn bản/MP3, đọc trạng thái thiết bị rồi nhập văn bản hoặc chọn MP3. Giữ hộp thoại mở đến khi truyền xong. Thiết bị chỉ xếp hàng phát sau khi nhận đủ và CRC hợp lệ. Trạng thái xếp hàng không đồng nghĩa đã phát xong.

## Bộ nhớ và truyền dữ liệu

| Nội dung | Có đủ PSRAM | Dùng RAM nội |
|---|---:|---:|
| Văn bản UTF-8 | 128 KiB | 16 KiB |
| MP3 | 2 MiB | 64 KiB |

Đây là trần ứng dụng. `uploadLimit` còn giới hạn theo khối bộ nhớ liên tục lớn nhất và giữ ngân sách cho audio, JSON và modem. Widget đọc giới hạn thực tế từ thiết bị; có thể thấp hơn bảng hoặc bằng 0.

Mỗi chunk chứa tối đa 1536 byte trước khi mã hóa base64. Offset ngăn ghi sai vị trí; gói gửi lại phải khớp dữ liệu đã nhận. CRC32 phát hiện lỗi dữ liệu, không thay thế xác thực hay mã hóa. Commit lặp lại cùng phiên không xếp hàng phát thêm lần nữa.

MP3 nằm trong RAM/PSRAM, mất sau khi tắt nguồn và được giải phóng sau phát/hủy. Chỉ một phiên upload hoạt động tại một thời điểm. File 2 MiB cần 1366 chunk, chưa tính begin/commit/retry; thời gian truyền phụ thuộc mạng và hạn mức RPC.

## Âm thanh và báo lửa

Văn bản được chia thành đoạn TTS tối đa 140 code point. Dịch vụ TTS có thể từ chối hoặc giới hạn yêu cầu; cache dùng LittleFS. MP3 được giải mã và đưa ra I2S 48 kHz, 16 bit. Không hỗ trợ đổi định dạng WAV/AAC thành MP3 chỉ bằng cách đổi đuôi file.

Báo lửa cần đầu vào tác động ổn định 100 ms; chỉ xóa cảnh báo sau 5 giây đầu vào không tác động. Cách này giảm kích hoạt do xung ngắn và tránh bật/tắt báo động liên tục. Báo lửa ngắt nội dung đang phát; nội dung bị ngắt không tự tiếp tục.

Âm lượng 0–100%, khởi động 100% tín hiệu số. Mức này không đồng nghĩa công suất sạch tối đa; cần đo clipping của ampli. Còi báo lửa có mức riêng. Thanh âm lượng gộp thao tác trong 350 ms; giao dịch modem đang chạy có thể làm phản hồi chậm.

## Xử lý lỗi

- Mã -12 với HTTPS ThingsBoard: đường TLS này chưa được triển khai.
- Không đọc được trạng thái: kiểm tra cấu hình, quyền RPC, thiết bị đích và kết nối modem.
- Giới hạn upload thấp: kiểm tra PSRAM, phiên đang chạy và bộ nhớ; thử file nhỏ hơn.
- `busy_fire_or_not_enough_memory`: kiểm tra báo lửa, phiên nhận/phát và bộ nhớ khả dụng.
- `invalid_or_unsupported_mp3`: thử file MP3 hợp lệ khác.
- Đã nhận nhưng không có tiếng: kiểm tra `audio_ok`, trạng thái phát, I2S, DAC và nguồn ampli.

Kiểm thử host và phạm vi chưa kiểm chứng được ghi tại `tests/VALIDATION.md`.
