# text-mp3-v2

- Loại bỏ YouTube, Windows Bridge, media URL và RPC playMedia.
- Thêm beginUpload/uploadChunk/commitUpload/cancelUpload cho văn bản UTF-8 và MP3.
- Base64 từng gói 1.536 byte, xác nhận offset, kiểm tra CRC32, chống nối/phát trùng khi retry.
- Đọc giới hạn từ bộ nhớ khả dụng; trần PSRAM 128 KiB văn bản, 2 MiB MP3.
- Không nhân đôi toàn bộ văn bản dài trong String, chia đoạn 140 code point.
- Xử lý ID3v2 trước khi giải mã MP3; giải phóng buffer sau phát/hủy.
- Giữ volume 0–100%, mặc định 100%; sửa reset audioOK khi khởi tạo lại I2S.
- Giữ cảnh báo lửa ưu tiên cục bộ, cảm biến, telemetry và cache TTS.
