# Panduan Konfigurasi Serial (ESP-NOW Only)

Dokumen ini menjelaskan cara mengatur device lewat serial pada firmware terbaru (`src/RFID_LA_ARDUINO_IDE.ino`).

## 1. Persiapan

1. Hubungkan ESP32 ke PC via USB.
2. Buka Serial Monitor dengan baud `115200`.
3. Kirim perintah satu baris per command (akhiri newline).

## 2. Daftar Command

- `HELP`
- `GET CONFIG`
- `SAVE`
- `REBOOT`
- `SET ROLE <NODE|GATEWAY>`
- `SET SLOT <1..64>`
- `SET GATEWAY_MAC <AA:BB:CC:DD:EE:FF>`
- `SET ACTION_MS <1000..120000>`
- `SET BUTTON_TIMEOUT_MS <1000..300000>`
- `SET RELAY2_ACTIVE <HIGH|LOW>`
- `SET RELAY4_ACTIVE <HIGH|LOW>`
- `SET RELAY22_ACTIVE <HIGH|LOW>`

## 3. Response Device

- Sukses: `OK ...`
- Gagal: `ERR ...`
- Dump konfigurasi: `CONFIG {...}`

## 4. Urutan Konfigurasi Disarankan

1. Set role:
   - `SET ROLE NODE` atau `SET ROLE GATEWAY`
2. Set nomor docking/loket:
   - `SET SLOT 3`
3. Jika role node, set MAC gateway:
   - `SET GATEWAY_MAC 24:6F:28:AA:BB:CC`
4. Set level output:
   - `SET RELAY2_ACTIVE HIGH`
   - `SET RELAY4_ACTIVE HIGH`
   - `SET RELAY22_ACTIVE LOW`
5. Cek:
   - `GET CONFIG`
6. Simpan permanen:
   - `SAVE`
7. Terapkan role/setting dari boot:
   - `REBOOT`

## 5. Contoh Sesi Cepat

```text
SET ROLE NODE
SET SLOT 12
SET GATEWAY_MAC AA:BB:CC:DD:EE:FF
SET RELAY2_ACTIVE HIGH
SET RELAY4_ACTIVE HIGH
SET RELAY22_ACTIVE LOW
GET CONFIG
SAVE
REBOOT
```

## 6. Catatan Penting

- Format MAC wajib `XX:XX:XX:XX:XX:XX`.
- Gunakan nilai `STA MAC (pakai untuk SET GATEWAY_MAC)` yang tercetak saat boot, bukan eFuse MAC.
- Perubahan belum persisten sampai `SAVE`.
- Perubahan role efektif penuh setelah `REBOOT`.
- `SET SLOT` valid di rentang `1..64`.
- ESP-NOW pada firmware ini memakai fixed channel `1`, jadi node dan gateway harus sama-sama firmware ini.
