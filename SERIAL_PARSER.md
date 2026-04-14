# Dokumentasi Parser Serial Gateway

Dokumen ini menjelaskan format komunikasi serial antara PC dan gateway ESP32 pada mode uplink `serial` atau `both`.

## Ringkasan

- Transport: UART `Serial` (USB CDC).
- Framing: **1 JSON object per baris** (diakhiri `\n`).
- Gateway membaca per karakter, mengabaikan `\r`, dan memproses saat `\n`.
- Panjang buffer baris masuk dibatasi ~400 karakter.

## Konfigurasi

Di dashboard web:

- `Device Role` = `gateway`
- `Gateway Uplink`:
  - `mqtt` = hanya MQTT
  - `serial` = hanya serial
  - `both` = MQTT + serial

## Format Pesan Masuk (PC -> Gateway)

Format umum:

```json
{"topic":"<topic>","payload":{...}}
```

Field wajib:

- `topic` (string)
- `payload` (JSON object)

Jika format tidak valid, gateway log:

```text
Serial command invalid
```

## Topik yang Didukung (Masuk)

Parser serial memakai handler yang sama dengan MQTT gateway, jadi topik yang didukung sama:

1. Status arm ke node

```json
{"topic":"boseh/status/12","payload":{"rfid_tag":"A1B2C3D4E5F6"}}
```

2. Control solenoid ke node

```json
{"topic":"boseh/device/12/control","payload":{"command":"solenoid","value":true}}
```

3. Maintenance request ke node

```json
{"topic":"boseh/12","payload":{"status":true}}
```

## Format Pesan Keluar (Gateway -> PC)

Semua event keluar via serial memakai format:

```json
{"topic":"<topic>","payload":{...}}
```

Event yang dikirim:

1. Confirm open

Topic:

```text
boseh/stasiun/confirm_open
```

Contoh payload:

```json
{"slot_number":12,"rfid_tag":"A1B2C3D4E5F6","status":true}
```

2. Maintenance status

Topic:

```text
boseh/maintenance
```

Contoh payload:

```json
{"slot_number":12,"ip_address":"192.168.1.20","status":true,"solenoid":false,"rfid_tag":"A1B2C3D4E5F6"}
```

## Catatan Perilaku Parser

- Parser `payload` mengharuskan object (`{...}`), bukan string/array.
- Spasi/tab setelah `:` diperbolehkan.
- Nested object di `payload` didukung selama JSON seimbang.
- Escape string dasar didukung saat scanning object.
- Jika baris terlalu panjang, karakter setelah batas buffer diabaikan sampai `\n`.

## Contoh Alur Uji Cepat

Kirim dari PC ke serial monitor gateway:

```json
{"topic":"boseh/device/1/control","payload":{"command":"solenoid","value":true}}
```

Jika valid dan slot peer tersedia, gateway akan meneruskan ke node lewat ESP-NOW.

## Troubleshooting

1. Tidak ada respons serial:
- Pastikan `Device Role = gateway`.
- Pastikan `Gateway Uplink = serial` atau `both`.
- Pastikan baud monitor sama (`115200`).

2. Muncul `Serial command invalid`:
- Cek JSON valid.
- Cek ada `topic` string.
- Cek `payload` adalah object.
- Cek pesan diakhiri newline (`\n`).

3. Command tidak sampai node:
- Cek slot sudah pernah heartbeat/terdaftar peer.
- Cek jarak/rf link ESP-NOW.
- Cek log `Peer slot tidak tersedia`.
