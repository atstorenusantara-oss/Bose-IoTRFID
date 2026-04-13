# Dokumentasi Perangkat RFID Boseh (ESP32 + ID-12LA)

Dokumen ini mengikuti implementasi terbaru pada `src/RFID_LA_ARDUINO_IDE.ino`.
Fitur utama: pembacaan RFID ID-12LA, kontrol output/solenoid, dashboard konfigurasi web, OTA update, dan komunikasi MQTT.

## 1. Ringkasan Fitur

- Baca RFID dari UART2 (`RX GPIO16`) dan pin deteksi range (`GPIO17`)
- Validasi frame RFID dengan checksum XOR (10 hex tag + 2 hex checksum)
- Publish event RFID/aksi ke MQTT
- Subscribe topic kontrol dan permintaan status maintenance
- Dashboard web untuk:
  - unlock PIN
  - set WiFi, MQTT IP, slot
  - set level aktif relay (HIGH/LOW)
  - debug GPIO
  - upload firmware OTA (`.bin`)
- AP fallback otomatis saat gagal konek WiFi STA

## 2. Mapping Pin Perangkat

| Fungsi | GPIO | Keterangan |
| :--- | :--- | :--- |
| RFID D0 (UART RX) | 16 | ID-12LA pin 9 |
| Tag In Range | 17 | ID-12LA pin 6 |
| LED status RFID | 2 | `LED_status` |
| Tombol input | 23 | `BUTTON_PIN`, `INPUT_PULLUP` |
| LED tombol | 15 | Aktif LOW |
| Output aksi / relay | 4 | `LED_ACTION` |
| LED indikator mirror | 22 | `LED_ACTION_INV` |

## 3. Konfigurasi Default

- WiFi SSID: `TUBIS43LT2`
- WiFi Password: `12345678`
- MQTT Broker: `192.168.0.177`
- MQTT Port: `1883`
- Slot default: `1`
- PIN dashboard: `1221`
- AP fallback SSID: `BOSEH-Config-{slot_number}`
- AP fallback password: `12345678`

Semua konfigurasi tersimpan di NVS (`Preferences`) namespace `cfg`.

## 4. MQTT (Aktual di Firmware)

### 4.1 Topic Subscribe (device menerima)

- `boseh/status/{slot_number}`
  - Dipakai untuk status slot (ambil `rfid_tag`)
- `boseh/device/{slot_number}/control`
  - Dipakai untuk command kontrol solenoid
- `boseh/{slot_number}`
  - Dipakai untuk trigger kirim status maintenance (`{"status":true}`)

### 4.2 Topic Publish (device mengirim)

- `boseh/stasiun/confirm_open`
  - Payload:
  ```json
  {
    "slot_number": 1,
    "rfid_tag": "A1B2C3D4E5FF",
    "status": true
  }
  ```
- `boseh/maintenance`
  - Payload:
  ```json
  {
    "slot_number": 1,
    "ip_address": "192.168.1.20",
    "status": true,
    "solenoid": false,
    "rfid_tag": "A1B2C3D4E5FF"
  }
  ```
- `boseh/ready`
  - Format:
  ```json
  {
    "bike_id": "..."
  }
  ```
  Catatan: fungsi publish ready tersedia di kode, tetapi belum dipanggil di alur utama saat ini.

### 4.3 Format Payload Subscribe

1. Topic `boseh/device/{slot}/control`
```json
{
  "slot_number": 1,
  "command": "solenoid",
  "value": true
}
```

2. Topic `boseh/{slot}`
```json
{
  "status": true
}
```

## 5. Alur Operasi Firmware

1. `setup()`:
   - init serial, RFID serial, pin
   - load config dari NVS
   - konek WiFi (dengan fallback AP)
   - start web server
   - konek MQTT + subscribe topic

2. `loop()`:
   - layani HTTP client
   - jika AP fallback aktif: loop utama RFID/MQTT berhenti sementara (return cepat)
   - jika mode normal: jaga koneksi WiFi + MQTT
   - baca status tag in range (`GPIO17`)
   - baca frame RFID dari UART dan validasi checksum
   - publish `confirm_open` saat event tertentu
   - proses tombol fisik dan timer aksi 10 detik

## 6. Trigger AP Fallback

AP fallback aktif saat:
- startup `setup()` gagal konek WiFi STA (`connectWiFi(true)`)
- setelah simpan konfigurasi (`/save`) lalu reconnect gagal (`connectWiFi(true)`)

Saat AP aktif, device tetap melayani dashboard web dan konfigurasi, tetapi logika utama pada `loop()` berhenti sementara sampai kondisi berubah.

## 7. Dashboard Web

### Endpoint utama

- `GET /`:
  - jika belum unlock -> halaman PIN
  - jika sudah unlock -> dashboard konfigurasi
- `POST /unlock`: verifikasi PIN
- `POST /save`: simpan config WiFi/MQTT/slot/relay level, lalu reconnect
- `POST /debug/pin`: baca/tulis mode dan level GPIO debug yang diizinkan
- `POST /update`: upload firmware OTA dan reboot saat sukses

### Mekanisme lock

- Dashboard memakai lock sederhana berbasis PIN (`1221`)
- Status unlock disimpan di RAM (`dashboardUnlocked`), bukan session token persisten

## 8. Event `confirm_open` (Perilaku Saat Ini)

Topic `boseh/stasiun/confirm_open` dipakai untuk beberapa event:
- `status=true` saat kartu terbaca dalam range
- `status=false` saat kartu keluar range
- `status=false` saat tombol ditekan untuk aksi open

Catatan integrasi backend: `status=false` memiliki lebih dari satu konteks event, jadi jika perlu pemisahan konteks sebaiknya tambah field event terpisah (mis. `event":"tag_out"` / `event":"button_open"`).

## 9. Build Environment

`platformio.ini`:
- `platform = espressif32`
- `board = esp32dev`
- `framework = arduino`
- `monitor_speed = 115200`
- dependency: `knolleary/PubSubClient`

## 10. Catatan Integrasi

- Dokumen lama yang memakai topic `boseh/stasiun/update` tidak lagi sesuai dengan firmware ini.
- Integrasi backend/distributor message harus mengikuti daftar topic pada bagian MQTT (bagian 4).
