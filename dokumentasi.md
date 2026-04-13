# Dokumentasi Perangkat RFID Boseh (ESP32 + ID-12LA)

Dokumen ini mengikuti implementasi terbaru pada `src/RFID_LA_ARDUINO_IDE.ino`.

## 1. Ringkasan Fitur

- Pembacaan RFID ID-12LA via UART2 (`GPIO16`) + pin range (`GPIO17`)
- Validasi data RFID berbasis checksum XOR (10 hex tag + 2 hex checksum)
- Dashboard web untuk konfigurasi WiFi, MQTT, slot, level relay, dan mode IP (`DHCP/STATIC`)
- OTA firmware update dari browser
- Integrasi MQTT subscribe/publish untuk status, kontrol solenoid, dan maintenance
- AP fallback otomatis bila koneksi WiFi STA gagal

## 2. Mapping Pin

| Fungsi | GPIO | Keterangan |
| :--- | :--- | :--- |
| RFID D0 (UART RX) | 16 | Dari ID-12LA pin 9 |
| Tag in Range | 17 | Dari ID-12LA pin 6 |
| LED status kartu | 4 | `LED_status` |
| Tombol | 23 | `BUTTON_PIN` (INPUT_PULLUP) |
| LED tombol | 15 | Aktif LOW |
| LED aksi/relay | 2 | `LED_ACTION` |
| LED indikator mirror | 22 | `LED_ACTION_INV` |

## 3. Konfigurasi Default

- WiFi SSID: `TUBIS43LT2`
- WiFi Password: `12345678`
- MQTT Broker: `192.168.0.113`
- MQTT Port: `1883`
- Slot Number: `1`
- AP fallback SSID: `BOSEH-Config-{slot_number}`
- AP fallback password: `12345678`
- Mode IP STA default: `DHCP`

Semua konfigurasi disimpan di NVS (`Preferences`) namespace `cfg`.

## 4. Konfigurasi IP STA (DHCP / Static)

Dashboard menyediakan pilihan:
- `DHCP`: IP didapat dari router
- `STATIC`: IP manual dengan parameter:
  - `Static IP`
  - `Gateway`
  - `Subnet Mask`
  - `DNS` (opsional)

### Validasi Dasar Static IP

Saat simpan config (`POST /save`) dan saat koneksi WiFi dijalankan:
- `Static IP`, `Gateway`, `Subnet` harus format IPv4 valid
- Nilai `0.0.0.0` ditolak
- `DNS` valid jika diisi

Jika konfigurasi static tidak valid saat koneksi:
- koneksi WiFi tidak dijalankan
- jika `allowApFallback=true`, device masuk AP fallback

## 5. MQTT (Aktual di Firmware)

### 5.1 Subscribe

- `boseh/status`
- `boseh/device/{slot_number}/control`
- `boseh/{slot_number}`

### 5.2 Publish

- `boseh/ready`
- `boseh/stasiun/confirm_open`
- `boseh/maintenance`

### 5.3 Contoh Payload

Kontrol solenoid (`boseh/device/{slot}/control`):
```json
{
  "slot_number": 1,
  "command": "solenoid",
  "value": true
}
```

Request maintenance (`boseh/{slot}`):
```json
{
  "status": true
}
```

Publish confirm open (`boseh/stasiun/confirm_open`):
```json
{
  "slot_number": 1,
  "rfid_tag": "A1B2C3D4E5FF",
  "status": true
}
```

Publish maintenance (`boseh/maintenance`):
```json
{
  "slot_number": 1,
  "ip_address": "192.168.1.50",
  "status": true,
  "solenoid": false,
  "rfid_tag": "A1B2C3D4E5FF"
}
```

## 6. Dashboard Web

Endpoint:
- `GET /` -> halaman konfigurasi
- `POST /save` -> simpan config + reconnect WiFi/MQTT
- `POST /update` -> upload OTA dan reboot bila sukses

Field konfigurasi:
- WiFi SSID
- WiFi Password
- MQTT Broker IP
- Slot Number
- IP Mode (`DHCP/STATIC`)
- Static IP, Gateway, Subnet, DNS
- Relay Pin2 Active Level
- Relay Pin22 Active Level

## 7. AP Fallback

AP fallback aktif saat:
- startup gagal konek WiFi (`connectWiFi(true)`)
- setelah save config jika reconnect gagal (`connectWiFi(true)`)

Saat AP aktif:
- dashboard web tetap jalan
- loop utama RFID/MQTT berhenti sementara (`apModeActive` -> return cepat)

## 8. Alur Operasi Singkat

1. `setup()`:
   - init serial/pin
   - load config NVS
   - konek WiFi (DHCP atau static sesuai config)
   - jalankan web server
   - konek MQTT

2. `loop()`:
   - handle client web
   - jaga koneksi WiFi/MQTT
   - baca status range tag + data RFID
   - publish event MQTT sesuai kondisi
   - proses tombol dan timer aksi 10 detik

## 9. Build Environment

`platformio.ini`:
- `platform = espressif32`
- `board = esp32dev`
- `framework = arduino`
- `monitor_speed = 115200`
- `lib_deps = knolleary/PubSubClient`
