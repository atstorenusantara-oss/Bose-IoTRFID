# Redesign Firmware Modbus-Only V2 (Tanpa WiFi)

Dokumen V2 ini adalah spesifikasi implementasi dan commissioning untuk migrasi firmware ke arsitektur **Modbus RTU only**.

## 1. Scope V2

1. Semua komunikasi eksternal memakai Modbus RTU.
2. Semua konfigurasi runtime + persist dilakukan via register Modbus.
3. Tidak ada WiFi, MQTT, ESP-NOW, Web dashboard, dan OTA web.
4. Fungsi inti RFID, tombol, relay/solenoid tetap dipertahankan.

## 2. Arsitektur Target

1. ESP32 bertindak sebagai Modbus RTU Slave.
2. PLC/IPC/SCADA bertindak sebagai Modbus RTU Master.
3. Physical link: RS485 half-duplex (A/B/GND) dengan transceiver (contoh MAX485/SP3485).
4. Firmware single-role: `modbus_node`.

## 3. Parameter Komunikasi Default

1. Protocol: Modbus RTU.
2. Slave ID default: `1`.
3. Baud default: `9600`.
4. Data bits: `8`.
5. Parity default: `None`.
6. Stop bit default: `1`.
7. Timeout response target: `< 100 ms` untuk read ringan.

## 4. Konvensi Register

1. Dokumen memakai dua notasi:
- `Offset` = alamat 0-based untuk implementasi library.
- `Ref` = notasi umum Modbus (`0xxxx`, `1xxxx`, `3xxxx`, `4xxxx`).

2. Endianness:
- Tiap register 16-bit.
- Nilai 32-bit disimpan `LO` lalu `HI` (little-word order).

3. String ASCII:
- 2 karakter per register.
- Jika sisa ganjil, byte terakhir diisi `0x00`.

## 5. Register Map Final Draft V2

### 5.1 Coil (RW, FC01/05/15)

| Offset | Ref | Nama | Akses | Default | Keterangan |
|---|---|---|---|---|---|
| 0 | 00001 | `CMD_SOLENOID` | R/W | 0 | `1=ON`, `0=OFF` |
| 1 | 00002 | `CMD_RESET_FAULT` | W pulse | 0 | Tulis `1`, firmware auto-clear ke `0` |
| 2 | 00003 | `CMD_SAVE_CONFIG` | W pulse | 0 | Tulis `1` untuk commit config ke Preferences |
| 3 | 00004 | `CMD_REBOOT` | W pulse | 0 | Tulis `1`, reboot terjadwal (delay pendek) |
| 4 | 00005 | `CMD_CLEAR_RFID_BUFFER` | W pulse | 0 | Hapus RFID terakhir dan flag valid |

### 5.2 Discrete Inputs (RO, FC02)

| Offset | Ref | Nama | Default | Keterangan |
|---|---|---|---|---|
| 0 | 10001 | `STS_TAG_IN_RANGE` | 0 | Status pin range RFID |
| 1 | 10002 | `STS_BUTTON_PRESSED` | 0 | Tombol aktif low, dipublikasi sebagai active high |
| 2 | 10003 | `STS_SOLENOID_ACTIVE` | 0 | State output solenoid aktual |
| 3 | 10004 | `STS_ACTION_ACTIVE` | 0 | State timer action sedang aktif |
| 4 | 10005 | `STS_CONFIG_DIRTY` | 0 | Ada config berubah tapi belum di-save |
| 5 | 10006 | `STS_FAULT_ACTIVE` | 0 | Fault summary |
| 6 | 10007 | `STS_UNLOCK_VALID` | 0 | Session unlock config aktif |
| 7 | 10008 | `STS_RFID_VALID` | 0 | RFID terakhir valid checksum |

### 5.3 Input Registers (RO, FC04)

| Offset | Ref | Nama | Tipe | Keterangan |
|---|---|---|---|---|
| 0 | 30001 | `STS_SLOT_NUMBER` | `uint16` | Slot aktif |
| 1 | 30002 | `STS_LAST_EVENT_CODE` | `uint16` | Kode event terakhir |
| 2 | 30003 | `STS_FAULT_CODE` | `uint16` | Kode fault aktif utama |
| 3 | 30004 | `STS_UPTIME_MS_LO` | `uint16` | Uptime ms low word |
| 4 | 30005 | `STS_UPTIME_MS_HI` | `uint16` | Uptime ms high word |
| 5 | 30006 | `STS_ACTION_REMAIN_MS_LO` | `uint16` | Sisa timer action low |
| 6 | 30007 | `STS_ACTION_REMAIN_MS_HI` | `uint16` | Sisa timer action high |
| 7 | 30008 | `STS_LAST_RFID_ASCII_0` | `char[2]` | RFID bytes 0..1 |
| 8 | 30009 | `STS_LAST_RFID_ASCII_1` | `char[2]` | RFID bytes 2..3 |
| 9 | 30010 | `STS_LAST_RFID_ASCII_2` | `char[2]` | RFID bytes 4..5 |
| 10 | 30011 | `STS_LAST_RFID_ASCII_3` | `char[2]` | RFID bytes 6..7 |
| 11 | 30012 | `STS_LAST_RFID_ASCII_4` | `char[2]` | RFID bytes 8..9 |
| 12 | 30013 | `STS_LAST_RFID_ASCII_5` | `char[2]` | RFID bytes 10..11 |
| 13 | 30014 | `STS_FW_VERSION_MAJOR` | `uint16` | Versi mayor |
| 14 | 30015 | `STS_FW_VERSION_MINOR` | `uint16` | Versi minor |
| 15 | 30016 | `STS_FW_VERSION_PATCH` | `uint16` | Versi patch |

### 5.4 Holding Registers (RW, FC03/06/16)

| Offset | Ref | Nama | Tipe | Default | Range/Enum |
|---|---|---|---|---|---|
| 0 | 40001 | `CFG_SLOT_NUMBER` | `uint16` | 1 | 1..64 |
| 1 | 40002 | `CFG_ACTION_MS_LO` | `uint16` | 10000 LSW | 1000..120000 ms |
| 2 | 40003 | `CFG_ACTION_MS_HI` | `uint16` | 0 MSW | 1000..120000 ms |
| 3 | 40004 | `CFG_WAIT_MS_LO` | `uint16` | 30000 LSW | 1000..300000 ms |
| 4 | 40005 | `CFG_WAIT_MS_HI` | `uint16` | 0 MSW | 1000..300000 ms |
| 5 | 40006 | `CFG_R4_ACTIVE_HIGH` | `uint16` | 1 | 0/1 |
| 6 | 40007 | `CFG_R2_ACTIVE_HIGH` | `uint16` | 1 | 0/1 |
| 7 | 40008 | `CFG_R22_ACTIVE_HIGH` | `uint16` | 0 | 0/1 |
| 8 | 40009 | `CFG_MODBUS_SLAVE_ID` | `uint16` | 1 | 1..247 |
| 9 | 40010 | `CFG_MODBUS_BAUD_ENUM` | `uint16` | 0 | 0:9600,1:19200,2:38400,3:57600,4:115200 |
| 10 | 40011 | `CFG_MODBUS_PARITY` | `uint16` | 0 | 0:none,1:even,2:odd |
| 11 | 40012 | `CFG_MODBUS_STOP_BITS` | `uint16` | 1 | 1/2 |
| 12 | 40013 | `CFG_APPLY_SERIAL_NOW` | `uint16` | 0 | Tulis `1` untuk apply serial |
| 13 | 40014 | `CFG_LOCAL_BUTTON_ENABLE` | `uint16` | 1 | 0:disable,1:enable |
| 14 | 40015 | `CFG_LOCAL_BUTTON_PRIORITY` | `uint16` | 0 | 0:normal,1:override |
| 30 | 40031 | `CFG_WRITE_UNLOCK_KEY` | `uint16` | 0 | Harus `0x5A5A` untuk write kritikal |
| 31 | 40032 | `CFG_UNLOCK_TIMEOUT_SEC` | `uint16` | 30 | 5..300 |

## 6. Event Code V2

| Kode | Nama |
|---|---|
| 0 | `EV_NONE` |
| 1 | `EV_BOOT` |
| 2 | `EV_TAG_ENTER` |
| 3 | `EV_TAG_LEAVE` |
| 4 | `EV_RFID_VALID` |
| 5 | `EV_RFID_INVALID` |
| 6 | `EV_BUTTON_PRESS` |
| 7 | `EV_SOLENOID_ON` |
| 8 | `EV_SOLENOID_OFF` |
| 9 | `EV_CONFIG_CHANGED` |
| 10 | `EV_CONFIG_SAVED` |
| 11 | `EV_REBOOT_REQUEST` |
| 12 | `EV_FAULT_SET` |
| 13 | `EV_FAULT_CLEARED` |

## 7. Fault Code V2

| Kode | Nama | Arti |
|---|---|---|
| 0 | `FLT_NONE` | Normal |
| 1 | `FLT_CFG_INVALID_RANGE` | Nilai konfigurasi di luar batas |
| 2 | `FLT_CFG_LOCKED` | Write config ditolak karena unlock belum valid |
| 3 | `FLT_RFID_FRAME` | Frame RFID tidak valid berulang |
| 4 | `FLT_RFID_UART_TIMEOUT` | UART RFID timeout |
| 5 | `FLT_RS485_BUS` | Error komunikasi RS485/Modbus internal |
| 6 | `FLT_STORAGE_WRITE` | Gagal simpan Preferences |

## 8. Aturan Write dan Keamanan Konfigurasi

1. Register config kritikal (`CFG_SLOT_NUMBER`, `CFG_MODBUS_*`) hanya boleh ditulis saat unlock valid.
2. Unlock valid jika master menulis `0x5A5A` ke `CFG_WRITE_UNLOCK_KEY`.
3. Unlock timeout mengikuti `CFG_UNLOCK_TIMEOUT_SEC`.
4. Setelah timeout, unlock auto-expire.
5. `CMD_SAVE_CONFIG` tanpa unlock valid harus ditolak dan set fault `FLT_CFG_LOCKED`.
6. Write nilai invalid harus ditolak dan set fault `FLT_CFG_INVALID_RANGE`.

## 9. State Machine Runtime V2

1. `INIT`: load preferences, setup GPIO/UART/Modbus, set `EV_BOOT`.
2. `IDLE`: poll RFID + tombol + Modbus.
3. `ARMED`: tag valid/range valid, menunggu aksi tombol/command.
4. `ACTION_ACTIVE`: solenoid ON, timer berjalan.
5. `FAULT`: fault aktif, tetap respons Modbus, output mengikuti fail-safe policy.

## 10. Commissioning Sequence (PLC)

1. Pastikan komunikasi serial match default (`ID=1`, `9600 8N1`).
2. Read `30001..30005` untuk cek device hidup.
3. Write unlock key (`40031=0x5A5A`).
4. Write konfigurasi yang dibutuhkan (`40001..40015`).
5. Trigger save (`00003=1`).
6. Verifikasi `10005=0` (`STS_CONFIG_DIRTY` cleared).
7. Jika parameter serial diubah, write `40013=1`, lalu reconnect master dengan parameter baru.
8. Uji command coil `00001` ON/OFF dan verifikasi `10003`.

## 11. Dampak Implementasi ke Kode

Komponen dihapus:
1. `WiFi.h`, `PubSubClient.h`, `WebServer.h`, `Update.h`, `esp_now.h`.
2. Semua fungsi koneksi AP/STA/MQTT/ESP-NOW/dashboard/OTA.

Komponen dipertahankan:
1. Parser RFID + checksum.
2. Logic tombol, LED, solenoid.
3. Penyimpanan `Preferences` untuk config persist.

Komponen baru:
1. Modul `modbus_map` (tabel register dan validator).
2. Modul `modbus_handler` (callback read/write).
3. Modul `rs485_hal` (DE/RE control + UART binding).
4. Modul `fault_manager`.

## 12. Rencana Migrasi Kode (Eksekusi)

1. Branch: `redesign/modbus-only`.
2. Commit A: bootstrap Modbus RTU slave + register read-only dasar.
3. Commit B: wiring runtime status ke discrete/input register.
4. Commit C: command coil (solenoid, reset fault, reboot, clear RFID).
5. Commit D: config RW + unlock + save ke Preferences.
6. Commit E: hapus dependensi WiFi/MQTT/ESP-NOW/Web.
7. Commit F: cleanup + test matrix + dokumentasi final register.

## 13. Test Matrix Minimum

1. Read/write register normal.
2. Write tanpa unlock (harus ditolak).
3. Write out-of-range (harus fault).
4. Save config lalu power cycle.
5. Ubah serial setting dan reconnect master.
6. Simulasi polling cepat dan burst write.
7. Uji RFID valid/invalid checksum.
8. Uji tombol lokal sesuai mode enable/priority.

## 14. Definition of Done V2

1. Firmware compile tanpa stack network.
2. Device fully operable via Modbus RTU.
3. Konfigurasi utama tersimpan dan survive reboot.
4. Register map stabil dan konsisten dengan implementasi.
5. Commissioning guide dapat dijalankan tim PLC tanpa perubahan besar.

## 15. Checklist Implementasi per Commit

Gunakan urutan ini agar perubahan tetap kecil, mudah diuji, dan mudah rollback.

Status saat ini (24 April 2026):
1. Commit A: selesai
2. Commit B: selesai
3. Commit C: selesai
4. Commit D: selesai
5. Commit E: selesai
6. Commit F: selesai

### 15.1 Commit A - Bootstrap Modbus RTU Slave

Tujuan:
1. Firmware bisa start sebagai Modbus slave.
2. Register read-only dasar bisa dibaca master.

Status: `DONE` (commit `abb99c9`)

Checklist perubahan:
1. Tambah dependency library Modbus di `platformio.ini`.
2. Tambah inisialisasi UART RS485 + pin DE/RE.
3. Buat skeleton modul:
- `src/modbus_map.h/.cpp`
- `src/modbus_handler.h/.cpp`
- `src/rs485_hal.h/.cpp`

4. Daftarkan minimal register:
- `STS_SLOT_NUMBER`
- `STS_UPTIME_MS_LO/HI`
- `STS_FW_VERSION_*`

5. Integrasikan polling `modbus.task()` ke `loop()`.

Checklist verifikasi:
1. Compile sukses.
2. Master bisa read FC04 untuk register dasar.
3. Tidak ada watchdog reset.

Commit message:
`feat(modbus): bootstrap RTU slave and basic input registers`

### 15.2 Commit B - Wiring Runtime Status

Tujuan:
1. Status realtime perangkat muncul di discrete/input register.

Status: `DONE` (commit `90eeb69`)

Checklist perubahan:
1. Mapping status pin ke discrete input:
- `STS_TAG_IN_RANGE`
- `STS_BUTTON_PRESSED`
- `STS_SOLENOID_ACTIVE`
- `STS_ACTION_ACTIVE`
- `STS_RFID_VALID`

2. Mapping RFID terakhir ke `STS_LAST_RFID_ASCII_*`.
3. Update `STS_LAST_EVENT_CODE` saat event runtime.
4. Tambah helper serializer ASCII 12 char ke 6 register.

Checklist verifikasi:
1. Polling Modbus mencerminkan perubahan fisik tombol/range.
2. RFID valid mengisi register string dan flag valid.
3. Event code berubah sesuai aksi.

Commit message:
`feat(modbus): expose runtime device states and RFID status registers`

### 15.3 Commit C - Command Coil

Tujuan:
1. Kontrol aksi utama via coil berfungsi.

Status: `DONE` (commit `7ca55ea`)

Checklist perubahan:
1. Implement handler write coil:
- `CMD_SOLENOID`
- `CMD_RESET_FAULT`
- `CMD_REBOOT`
- `CMD_CLEAR_RFID_BUFFER`

2. Pulse coil auto-reset ke `0` untuk command pulse.
3. Tambah fail-safe behavior saat fault aktif (sesuai policy).

Checklist verifikasi:
1. Write `00001` ON/OFF mengubah output fisik.
2. `CMD_REBOOT` memicu restart terkontrol.
3. `CMD_CLEAR_RFID_BUFFER` membersihkan RFID register.

Commit message:
`feat(modbus): implement command coils for solenoid and system actions`

### 15.4 Commit D - Config RW + Unlock + Persist

Tujuan:
1. Konfigurasi bisa ditulis aman via Modbus dan disimpan persist.

Status: `DONE` (commit `91abf4d`)

Checklist perubahan:
1. Implement holding register RW (`40001..40015`, `40031..40032`).
2. Implement unlock session (`0x5A5A` + timeout).
3. Validasi range semua parameter config.
4. Implement `CMD_SAVE_CONFIG` ke `Preferences`.
5. Set/clear `STS_CONFIG_DIRTY`.

Checklist verifikasi:
1. Write config tanpa unlock ditolak + fault `FLT_CFG_LOCKED`.
2. Write invalid ditolak + fault `FLT_CFG_INVALID_RANGE`.
3. Save valid persist setelah reboot.

Commit message:
`feat(modbus): add secure config registers with unlock and persistent save`

### 15.5 Commit E - Remove Legacy Network Stack

Tujuan:
1. Codebase bersih dari WiFi/MQTT/ESP-NOW/Web.

Status: `DONE` (commit `5277d80`)

Checklist perubahan:
1. Hapus include dan fungsi:
- WiFi/MQTT/AP fallback
- ESP-NOW queue/bridge
- Web dashboard/OTA

2. Sederhanakan `setup()` dan `loop()` untuk runtime lokal + Modbus.
3. Pastikan parser RFID + tombol + relay tetap utuh.

Checklist verifikasi:
1. Compile sukses tanpa dependency network lama.
2. Fungsi RFID + tombol + solenoid tetap berjalan.
3. Semua I/O eksternal lewat Modbus.

Commit message:
`refactor(core): remove legacy wifi mqtt espnow and web subsystems`

### 15.6 Commit F - Hardening + Test + Final Docs

Tujuan:
1. Menutup gap kualitas sebelum release.

Status: `DONE`

Checklist perubahan:
1. Tambah guard error RS485/Modbus.
2. Rapikan fault manager dan logging serial debug.
3. Finalisasi register map agar identik dengan implementasi.
4. Tambah dokumen quick-start commissioning.

Checklist verifikasi:
1. Jalankan test matrix section 13.
2. Uji power cycle berulang.
3. Uji perubahan serial parameter dan reconnect master.

Commit message:
`chore(release): harden modbus runtime and finalize commissioning docs`

### 15.7 Gate Checklist Sebelum Merge

1. Semua commit di atas lolos compile.
2. Test matrix section 13 ditandai pass/fail dengan catatan.
3. Tidak ada referensi sisa ke `WiFi`, `PubSubClient`, `WebServer`, `esp_now`.
4. Register map dokumen sama persis dengan offset di kode.
5. Dilakukan smoke test dengan minimal 1 tool Modbus master.

## 16. As-Built Snapshot (Setelah Commit E)

Section ini mencatat kondisi implementasi aktual di kode agar dokumen selaras dengan firmware saat ini.

1. Library Modbus aktif: `emelianov/modbus-esp8266`.
2. Modbus RTU slave aktif dengan default:
- Slave ID: `1`
- Serial: `9600 8N1`

3. RS485 HAL saat ini:
- UART: `HardwareSerial(1)`
- RX pin: `25`
- TX pin: `26`
- DE/RE pin: `21`

4. Register yang sudah aktif:
- Coil `0..4`: `CMD_SOLENOID`, `CMD_RESET_FAULT`, `CMD_SAVE_CONFIG`, `CMD_REBOOT`, `CMD_CLEAR_RFID_BUFFER`
- Discrete input `0..7`: status runtime utama
- Input register `0..15`: slot, event, fault, uptime, action remain, RFID ASCII, versi firmware
- Holding register config (`4xxxx`) sesuai tabel 5.4, termasuk unlock key dan timeout

5. Perilaku command coil yang sudah berjalan:
- `CMD_SOLENOID`: mengubah output dan state aksi.
- `CMD_RESET_FAULT`: clear fault lokal (`fault_active=false`, `fault_code=0`).
- `CMD_SAVE_CONFIG`: menyimpan config aktif ke `Preferences`.
- `CMD_REBOOT`: restart device.
- `CMD_CLEAR_RFID_BUFFER`: hapus RFID cache runtime.

6. Gap yang masih ada dibanding target V2:
- Uji commissioning end-to-end Modbus master pada perangkat fisik masih perlu dieksekusi.
- Soak test stabilitas jangka panjang masih perlu dijalankan.

7. Legacy stack yang sudah dihapus dari firmware utama:
- WiFi
- MQTT
- ESP-NOW
- Web dashboard
- OTA web

## 17. Fokus Langkah Berikutnya (Post-Commit F)

1. Jalankan commissioning hardware RS485 dengan master nyata (PLC/SCADA).
2. Lengkapi bukti test matrix untuk item yang masih `TODO`.
3. Tentukan keputusan final release candidate berdasarkan hasil soak test.
