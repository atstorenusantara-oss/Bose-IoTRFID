# Desain ESP-NOW Only + Provisioning Konfigurasi Loket

Dokumen ini mendesain arsitektur agar operasi utama tidak bergantung pada WiFi STA/MQTT, dengan konfigurasi loket dilakukan lewat ESP-NOW (utama) dan Serial USB (fallback).

## 1. Tujuan

- Menghilangkan delay akibat reconnect WiFi/MQTT blocking pada loop real-time.
- Menjadikan komunikasi `gateway <-> node` sepenuhnya lewat ESP-NOW saat operasi harian.
- Menyediakan cara konfigurasi per loket tanpa internet/router.

## 2. Mode Operasi

### 2.1 Node Mode (Operasional)
- Node membaca RFID, tombol, dan output relay seperti saat ini.
- Semua event status/maintenance dikirim ke gateway via ESP-NOW.
- Tidak ada pemanggilan `connectWiFi()` dan `connectMQTT()` dalam loop node.

### 2.2 Gateway Mode (Operasional)
- Gateway memproses paket dari semua node via ESP-NOW.
- Uplink keluar hanya `serial` (USB/UART ke PC/host).
- Tidak ada koneksi WiFi STA dan tidak ada koneksi MQTT pada mode operasional.
- Jalur ESP-NOW tetap prioritas real-time.

### 2.3 Provisioning Mode
- Digunakan untuk set konfigurasi node:
  - `slot_number`
  - `gateway_mac`
  - `action_duration_ms`
  - `button_wait_timeout_ms`
  - level aktif relay (`pin2`, `pin4`, `pin22`)
- Provisioning dapat dilakukan:
  - lewat ESP-NOW command dari gateway
  - atau fallback via serial USB langsung ke node

## 3. Rancangan Protokol Provisioning ESP-NOW

## 3.1 Tambahan Tipe Paket

Tambahkan pada enum msgType:
- `ESPNOW_MSG_CONFIG_SET = 8`  (gateway -> node)
- `ESPNOW_MSG_CONFIG_GET = 9`  (gateway -> node)
- `ESPNOW_MSG_CONFIG_RESP = 10` (node -> gateway)

## 3.2 Skema Payload

Karena `EspNowPacket` saat ini fixed-size, gunakan pola berikut:

- `command`:
  - `"cfg_set"`
  - `"cfg_get"`
  - `"cfg_rsp"`
- `flags` bitmask field yang diubah:
  - bit0: `slot_number`
  - bit1: `gateway_mac`
  - bit2: `action_duration_ms`
  - bit3: `button_wait_timeout_ms`
  - bit4: `relay_pin2_active_high`
  - bit5: `relay_pin4_active_high`
  - bit6: `relay_pin22_active_high`
- `value`: nilai kecil (bool/slot 1..64)
- `rfidTag` dipakai sebagai string data ringkas (mis. MAC tanpa separator atau token angka).

Catatan: bila payload jadi kompleks, tahap 2 dapat menambah `struct EspNowConfigPacket` khusus.

## 3.3 Alur `CONFIG_SET`

1. Gateway kirim `ESPNOW_MSG_CONFIG_SET` ke node target (berdasarkan MAC/slot).
2. Node validasi data:
   - slot 1..64
   - format MAC gateway valid
   - timeout dan duration dalam batas aman.
3. Jika valid:
   - simpan ke RAM config aktif
   - `saveConfig(...)` ke NVS
   - kirim ACK + `CONFIG_RESP status=success`.
4. Jika invalid:
   - kirim ACK + `CONFIG_RESP status=failed` + kode error di `value`.

## 3.4 Alur `CONFIG_GET`

1. Gateway kirim `ESPNOW_MSG_CONFIG_GET`.
2. Node kirim `ESPNOW_MSG_CONFIG_RESP` berisi snapshot config aktif.
3. Gateway cetak ke serial JSON agar mudah dipakai tool PC.

## 4. Keamanan Minimal

- Node hanya menerima config dari `gateway_mac` yang sudah dipasangkan.
- Untuk bootstrap awal (gateway MAC belum ada), izinkan sekali lewat:
  - serial USB lokal, atau
  - mode pairing sementara dengan token PIN (opsional tahap 2).

## 5. Fallback Konfigurasi via Serial USB

Gunakan format 1 baris 1 command:

```text
SET SLOT 12
SET GATEWAY_MAC AA:BB:CC:DD:EE:FF
SET ACTION_MS 10000
SET BUTTON_TIMEOUT_MS 30000
SET RELAY2_ACTIVE HIGH
SET RELAY4_ACTIVE HIGH
SET RELAY22_ACTIVE LOW
GET CONFIG
SAVE
REBOOT
```

Node mengembalikan response singkat:
- `OK`
- `ERR <reason>`
- `CONFIG {...}`

## 6. Integrasi ke Kode Saat Ini

## 6.1 Perubahan Wajib
- Tambah handler msgType config di `handleNodeEspNowPacket(...)`.
- Tambah pengirim command config di sisi gateway (bisa dipicu dari serial parser gateway).
- Tambah serializer config response ke JSON line di gateway serial output.

## 6.2 Perubahan Disarankan (anti-delay)
- Buat flag global `wifiStaEnabled`:
  - default `false` untuk deployment ESP-NOW-only.
- Guard semua pemanggilan:
  - `connectWiFi(...)`
  - `connectMQTT()`
  - `mqttClient.loop()`
- Terapkan di `node` dan `gateway` (keduanya no WiFi/MQTT saat operasional).
- Hasil: loop ESP-NOW + serial tetap deterministik.

## 7. Contoh Alur Provisioning Nyata

1. Teknisi colok gateway ke PC.
2. Gateway menerima command serial:
```json
{"topic":"boseh/provision/12","payload":{"command":"cfg_set","slot_number":12,"gateway_mac":"AA:BB:CC:DD:EE:FF","action_ms":10000,"button_timeout_ms":30000}}
```
3. Gateway kirim `CONFIG_SET` via ESP-NOW ke node slot 12.
4. Node simpan config, kirim `CONFIG_RESP success`.
5. Gateway kirim balik ke PC:
```json
{"topic":"boseh/provision/12/resp","payload":{"status":true}}
```

## 8. Tahapan Implementasi

1. Tahap A (cepat):
- Nonaktifkan koneksi WiFi/MQTT pada mode node dan gateway.
- Uplink gateway fixed `serial`.
- Tambah `CONFIG_SET` minimal: slot + gateway_mac.

2. Tahap B:
- Lengkapi semua parameter config + validation + response code.
- Tambah `CONFIG_GET`.

3. Tahap C:
- Tambah proteksi bootstrap/pairing dan audit log.

## 9. Kriteria Sukses Uji

- Saat AP/router dimatikan total, node-gateway tetap responsif (ESP-NOW).
- RTT command control (gateway->node->ack) stabil dan tidak freeze detik-an.
- Provisioning slot baru berhasil tanpa internet.
- Setelah reboot, config tetap persisten dari NVS.
