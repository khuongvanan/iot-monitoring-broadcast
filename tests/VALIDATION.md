# Kiểm thử

## Kết quả kiểm tra trên máy tính

- Upload buffer: PASS — CRC32, offset, phiên/epoch, giới hạn bộ nhớ, gói lặp và dữ liệu lặp không khớp.
- Widget: PASS — văn bản UTF-8, giới hạn upload, truyền MP3, mất ACK, commit lặp và điều khiển âm lượng.
- Các JSON dashboard/widget parse hợp lệ.
- Chương trình test MP3 biên dịch thành công; kết quả chạy cần file MP3 đầu vào.

## Chạy lại

Từ thư mục gốc repository:

```bash
g++ -std=c++17 -O2 tests/test_upload.cpp -o /tmp/test_upload
/tmp/test_upload
node tests/test_widget.js
g++ -std=c++17 -O2 tests/test_mp3.cpp -o /tmp/test_mp3
/tmp/test_mp3 /path/to/sample.mp3
```

## Phạm vi chưa xác minh

Chưa xác minh lại toàn firmware bằng toolchain ESP32, bắt tay TLS trên modem, import dashboard vào ThingsBoard, ERC/DRC và vận hành trên phần cứng. Test host không thay thế kiểm thử tích hợp. HTTPS ThingsBoard hiện trả lỗi -12; xem docs/usage.md.
