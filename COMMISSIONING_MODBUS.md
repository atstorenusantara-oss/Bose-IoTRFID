# Modbus Commissioning Quick Start

Dokumen ini dipakai untuk commissioning firmware `modbus-only` pada jalur RS485 Modbus RTU.

## 1. Default Komunikasi

1. Slave ID: `1`
2. Serial: `9600 8N1`
3. Mode: Modbus RTU slave

## 2. Register Sanity Check

1. Baca `30001..30005`:
- `30001`: slot number
- `30002`: event code
- `30003`: fault code
- `30004..30005`: uptime

2. Baca `10001..10008` untuk status digital realtime.

3. Baca `40001..40015` dan `40031..40032` untuk konfigurasi aktif.

## 3. Alur Konfigurasi Aman

1. Write `40031 = 0x5A5A` (unlock key).
2. Write register config yang ingin diubah.
3. Verifikasi `10005` (`STS_CONFIG_DIRTY`) menjadi `1`.
4. Trigger save dengan write coil `00003 = 1`.
5. Verifikasi `10005` kembali `0`.

## 4. Apply Serial Policy

`CFG_APPLY_SERIAL_NOW` (`40013`) menggunakan kebijakan reboot:
1. Set `40013 = 1` melalui jalur config unlocked.
2. Firmware akan menjadwalkan reboot command, auto-reset `40013` ke `0`.
3. Jika ada config dirty, firmware autosave sebelum reboot.

## 5. Command Coil Smoke Test

1. `00001` ON/OFF: kontrol solenoid.
2. `00002 = 1`: reset fault.
3. `00004 = 1`: reboot.
4. `00005 = 1`: clear RFID buffer.

## 6. Fault Behavior

1. Write config saat unlock tidak valid -> `fault_code=2` (`FLT_CFG_LOCKED`).
2. Write config di luar range -> `fault_code=1` (`FLT_CFG_INVALID_RANGE`).
3. Reset fault via `00002`.

## 7. Test Matrix Status (Saat Ini)

| Item | Status | Catatan |
|---|---|---|
| Build firmware (`pio run`) | PASS | Build sukses setelah refactor modbus-only |
| Legacy stack removal | PASS | WiFi/MQTT/ESP-NOW/Web tidak dipakai di firmware utama |
| Register map A-D | PASS | Coil/ISTS/IREG/HREG utama aktif |
| Hardware RS485 bus test | TODO | Perlu uji di bench/device fisik |
| PLC interoperability | TODO | Perlu uji dengan master PLC target |
| Long-run stability | TODO | Soak test minimal 24 jam |
