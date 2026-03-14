// ================================================
// ESP32 + ID-12LA RFID Reader
// Menggunakan Pin 9 (D0) + Pin 6 (Tag in Range)
// ================================================

#define RX_PIN      16     // GPIO16 → RFID Pin 9 (D0)
#define RANGE_PIN   17     // GPIO17 → RFID Pin 6 (Tag in Range)
#define LED_Range 26 // GPIO26 → LED Indikator Range (Opsional)


HardwareSerial RFIDSerial(2);  // UART2 pada ESP32

String tagID = "";
bool tagDetected = false;

void setup() {
  Serial.begin(115200);           // Serial Monitor (USB)
  RFIDSerial.begin(9600, SERIAL_8N1, RX_PIN, -1);  // RX = GPIO16, TX tidak dipakai

  pinMode(RANGE_PIN, INPUT);
  pinMode(LED_Range, OUTPUT);

  Serial.println("\n=== ESP32 + ID-12LA RFID Reader Siap ===");
  Serial.println("Menunggu tag 125kHz...\n");

  digitalWrite(LED_Range, 1); // Matikan LED Range
}

void loop() {
  // ====================== DETEKSI RANGE (Pin 6) ======================
  bool inRange = digitalRead(RANGE_PIN) == HIGH;

  if (inRange && !tagDetected) {
    tagDetected = true;
    Serial.println("🔵 TAG MASUK RANGE!");
  }

  // ====================== BACA DATA TAG (Pin 9) ======================
  if (RFIDSerial.available() > 0) {
    digitalWrite(LED_Range, 0); // Nyalakan LED Range saat tag terbaca
    tagID = "";
    
    while (RFIDSerial.available()) {
      char c = RFIDSerial.read();
      if (c == '\r' || c == '\n') break;   // ID-12LA mengakhiri dengan CR/LF
      if (c >= '0' && c <= '9' || c >= 'A' && c <= 'F') {
        tagID += c;                        // Hanya ambil hex ID
      }
    }

    if (tagID.length() >= 10) {            // ID-12LA biasanya 10-12 karakter hex
      Serial.print("✅ TAG TERBACA     : ");
      Serial.println(tagID);
      Serial.print("   Scan Time     : ");
      Serial.print(millis());
      Serial.println(" ms");
      Serial.println("-----------------------------------");
    }
  }

  // Reset flag saat tag keluar range
  if (!inRange && tagDetected) {
      digitalWrite(LED_Range, 1); // Matikan LED Range saat tag keluar
    tagDetected = false;
    Serial.println("🔴 Tag keluar range\n");
  }

  delay(50);  // Anti spam loop
}