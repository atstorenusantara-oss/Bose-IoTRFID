# Dokumentasi Parser Serial Gateway

Dokumen ini mendefinisikan protokol serial antara aplikasi desktop dan gateway ESP32 pada mode uplink `serial` atau `both`.

## Ringkasan

- Transport: UART `Serial` (USB CDC), `115200 8N1`.
- Framing: **1 JSON object per baris**, wajib diakhiri `\n`.
- Gateway membaca per karakter, mengabaikan `\r`, memproses saat `\n`.
- Batas panjang baris input ke gateway sekitar 400 karakter.

## Konfigurasi Perangkat

Set di dashboard web gateway:

- `Device Role = gateway`
- `Gateway Uplink = serial` atau `both`

Jika `Gateway Uplink = mqtt`, jalur serial untuk command desktop tidak dipakai.

## Kontrak Data Serial

Semua arah komunikasi memakai envelope yang sama:

```json
{"topic":"<topic>","payload":{...}}
```

Aturan:

- `topic` wajib string.
- `payload` wajib object JSON (`{}`), bukan string/array.
- JSON harus valid dan selesai dalam satu baris.

Jika invalid, gateway menulis log:

```text
Serial command invalid
```

## Pesan Masuk (Desktop -> Gateway)

### 1) Arm status ke node

Topic: `boseh/status/{slot}`

```json
{"topic":"boseh/status/12","payload":{"rfid_tag":"A1B2C3D4E5F6"}}
```

### 2) Control solenoid ke node

Topic: `boseh/device/{slot}/control`

```json
{"topic":"boseh/device/12/control","payload":{"command":"solenoid","value":true}}
```

### 3) Maintenance request ke node

Topic: `boseh/{slot}`

```json
{"topic":"boseh/12","payload":{"status":true}}
```

## Pesan Keluar (Gateway -> Desktop)

### 1) Confirm open dari node

Topic: `boseh/stasiun/confirm_open`

```json
{"topic":"boseh/stasiun/confirm_open","payload":{"slot_number":12,"rfid_tag":"A1B2C3D4E5F6","status":true}}
```

Keterangan payload:

- `slot_number` (number): ID slot node.
- `rfid_tag` (string): RFID yang terbaca.
- `status` (bool): status event dari node.

### 2) Maintenance status dari node

Topic: `boseh/maintenance`

```json
{"topic":"boseh/maintenance","payload":{"slot_number":12,"ip_address":"192.168.1.20","status":true,"solenoid":false,"rfid_tag":"A1B2C3D4E5F6"}}
```

Keterangan payload:

- `slot_number` (number): ID slot node.
- `ip_address` (string): IP gateway saat publish event.
- `status` (bool): status node dari paket maintenance.
- `solenoid` (bool): status output solenoid node.
- `rfid_tag` (string): RFID terakhir yang diketahui node.

## Contoh Stream Serial untuk Aplikasi Desktop

Contoh tulis dari desktop (TX):

```text
{"topic":"boseh/device/1/control","payload":{"command":"solenoid","value":true}}\n
{"topic":"boseh/1","payload":{"status":true}}\n
```

Contoh baca di desktop (RX):

```text
{"topic":"boseh/stasiun/confirm_open","payload":{"slot_number":1,"rfid_tag":"A1B2C3D4E5F6","status":true}}
{"topic":"boseh/maintenance","payload":{"slot_number":1,"ip_address":"192.168.1.20","status":true,"solenoid":false,"rfid_tag":"A1B2C3D4E5F6"}}
```

## Rekomendasi Implementasi Desktop

- Baca serial sebagai stream teks dan buffer sampai `\n`.
- Abaikan `\r`, parse tiap baris sebagai JSON object.
- Validasi field `topic` dan `payload` sebelum diproses.
- Routing berdasarkan `topic` saja, isi detail ambil dari `payload`.
- Saat mengirim command, selalu tambahkan newline di akhir.

## Troubleshooting

1. Tidak ada respons serial:
- Pastikan role device adalah gateway.
- Pastikan uplink `serial` atau `both`.
- Pastikan baud desktop `115200`.

2. Muncul `Serial command invalid`:
- Pastikan JSON valid.
- Pastikan ada `topic` string.
- Pastikan `payload` object.
- Pastikan command diakhiri `\n`.

3. Command diterima tapi node tidak aksi:
- Cek slot node sudah online/terdaftar peer.
- Cek sinyal/range ESP-NOW.
- Cek log gateway: `Peer slot tidak tersedia`.
