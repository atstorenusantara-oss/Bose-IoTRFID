# Dokumentasi Integrasi IoT Device (RFID Boseh)

Dokumen ini menjelaskan cara menghubungkan perangkat IoT (ESP32/RFID Reader) ke sistem Dashboard Boseh menggunakan MQTT (Rekomendasi) atau HTTP API.

## 1. Integrasi via MQTT (Pilihan Utama)

Sistem telah dikonfigurasi menggunakan **Mosquitto MQTT Broker** (Lokal).

### Parameter Koneksi
- **Broker IP:** `[Alamat_IP_Laptop_Anda]` (Gunakan `ipconfig` di CMD untuk melihatnya)
- **Port:** `1883`
- **Topic:** `boseh/stasiun/update`
- **QoS:** `0` atau `1`

### Format Data (JSON Payload)
Kirimkan pesan ke topic di atas dengan format berikut:

```json
{
    "slot_number": 1,
    "rfid_tag": "ID-KARTU-123",
    "status": true
}
```

| Parameter | Tipe | Deskripsi |
| :--- | :--- | :--- |
| `slot_number` | Int | Nomor fisik slot (1, 2, 3, dst). |
| `rfid_tag` | String | ID Unik kartu RFID. Masukkan `null` jika kartu tidak ada. |
| `status` | Bool | `true` jika kartu menempel (Attached), `false` jika dilepas. |

---

## 2. Contoh Kode (Arduino/ESP32)

Pastikan Anda sudah menginstal library **PubSubClient** di Arduino IDE.

```cpp
#include <WiFi.h>
#include <PubSubClient.h>

const char* ssid = "WiFi_Anda";
const char* password = "Password_WiFi";
const char* mqtt_server = "192.168.1.10"; // GANTI DENGAN IP LAPTOP ANDA

WiFiClient espClient;
PubSubClient client(espClient);

void setup() {
  setup_wifi();
  client.setServer(mqtt_server, 1883);
}

void sendRfidUpdate(int slot, String rfid, bool isPresent) {
  if (!client.connected()) reconnect();
  
  String payload = "{\"slot_number\":" + String(slot) + 
                   ",\"rfid_tag\":\"" + rfid + 
                   "\",\"status\":" + (isPresent ? "true" : "false") + "}";
                   
  client.publish("boseh/stasiun/update", payload.c_str());
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();
  
  // Contoh penggunaan saat sensor mendeteksi kartu
  // sendRfidUpdate(1, "AABBCCDD", true);
}
```

---

## 3. Integrasi via HTTP (Opsional/Backup)

Jika tidak ingin menggunakan MQTT, sistem tetap mendukung HTTP POST.

- **URL:** `http://[IP_LAPTOP]:5000/api/iot/update`
- **Method:** `POST`
- **Header:** `Content-Type: application/json`

### Contoh Payload HTTP
```json
{
    "slot_number": 1,
    "rfid_tag": "BOS-ID-998877",
    "status": true
}
```

### Respon Server
- `200 OK`: Data diterima dan Dashboard otomatis update.
- `400 Bad Request`: Format data salah atau `slot_number` hilang.

---

## 4. Tips Debugging
1. **MQTT Explorer**: Gunakan aplikasi "MQTT Explorer" di laptop untuk memantau apakah data dari ESP32 benar-benar masuk.
2. **Ping Test**: Pastikan ESP32 bisa melakukan `ping` ke alamat IP laptop server.
3. **Firewall**: Pastikan Windows Firewall Anda mengizinkan koneksi port `1883` (MQTT) dan `5000` (Flask).
