# Dokumentasi ESP32 RFID + MQTT + Dashboard OTA

## Ringkasan Fitur
- Baca RFID ID-12LA via UART2.
- Subscribe MQTT topic `boseh/status`.
- Publish MQTT:
  - `boseh/ready`
  - `boseh/stasiun/confirm_open`
- Dashboard web untuk update konfigurasi tanpa edit kode.
- OTA firmware upload dari browser.
- Penyimpanan konfigurasi persisten dengan `Preferences` (NVS).

## Konfigurasi Yang Bisa Diubah Dari Dashboard
- `WiFi SSID`
- `WiFi Password`
- `MQTT Broker IP`
- `Slot Number`

Semua konfigurasi tersimpan permanen dan dipakai saat reboot berikutnya.

## Nilai Default Konfigurasi
- `WiFi SSID`: `TUBIS43LT2`
- `WiFi Password`: `12345678`
- `MQTT Broker IP`: `192.168.0.105`
- `MQTT Port`: `1883`
- `Slot Number`: `1`

## Akses Dashboard
1. Nyalakan ESP32 dan lihat Serial Monitor untuk IP.
2. Buka browser ke `http://<IP-ESP32>/`.
3. Ubah konfigurasi lalu klik `Simpan Config`.

Jika WiFi gagal terkoneksi, ESP32 otomatis masuk AP fallback:
- SSID: `ESP32-Config`
- Password: `12345678`
- IP AP: `192.168.4.1`

## OTA Firmware (Web)
1. Buka dashboard `http://<IP-ESP32>/`.
2. Di bagian `OTA Firmware Update`, pilih file `.bin`.
3. Klik `Upload OTA`.
4. Jika sukses, perangkat reboot otomatis.

## Alur MQTT
### Subscribe
- Topic: `boseh/status`
- Contoh payload:
```json
{"slot_number": 8, "rfid_tag": "00000008"}
```

### Publish `boseh/ready`
- Trigger: tombol ditekan dalam window validasi slot 60 detik.
- Payload:
```json
{"bike_id":"<rfid_tag dari boseh/status>"}
```

### Publish `boseh/stasiun/confirm_open`
- Trigger:
  - `status=true` saat RFID masuk jangkauan.
  - `status=false` saat RFID keluar jangkauan.
- Payload:
```json
{"slot_number": 8, "rfid_tag": "00000008", "status": true}
```
`slot_number` diambil dari konfigurasi `Slot Number` di dashboard.

## Mapping Pin
- `GPIO16`: RX dari ID-12LA (D0)
- `GPIO17`: Tag in Range
- `GPIO4`: LED status
- `GPIO23`: Tombol input
- `GPIO15`: LED tombol (aktif LOW)
- `GPIO2`: LED aksi (ON 10 detik saat tombol valid ditekan)

## Catatan Operasional
- Pastikan broker MQTT menerima koneksi dari IP ESP32.
- Format payload `boseh/status` harus memuat `slot_number` dan `rfid_tag`.
- Jika tombol ditekan tapi aksi tidak jalan, cek:
  - slot dari payload harus sama dengan `Slot Number` konfigurasi
  - masih dalam timeout 60 detik
  - wiring tombol ke pin yang benar (`GPIO23` saat ini)
