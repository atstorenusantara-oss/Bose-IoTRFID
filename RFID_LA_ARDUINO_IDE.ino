// ================================================
// ESP32 + ID-12LA RFID Reader
// Menggunakan Pin 9 (D0) + Pin 6 (Tag in Range)
// ================================================

#define RX_PIN      16     // GPIO16 → RFID Pin 9 (D0)
#define RANGE_PIN   17     // GPIO17 → RFID Pin 6 (Tag in Range)
#define LED_status 26     // GPIO26 → LED indikator (opsional)

HardwareSerial RFIDSerial(2);  // UART2 pada ESP32

String tagID = "";
bool tagDetected = false;

void setup() {
  Serial.begin(115200);           // Serial Monitor (USB)
  RFIDSerial.begin(9600, SERIAL_8N1, RX_PIN, -1);  // RX = GPIO16, TX tidak dipakai

  pinMode(RANGE_PIN, INPUT);
  pinMode(LED_status, OUTPUT);

  Serial.println("\n=== ESP32 + ID-12LA RFID Reader Siap ===");
  Serial.println("Menunggu tag 125kHz...\n");
  digitalWrite(LED_status, HIGH);  // Matikan LED saat start
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
       digitalWrite(LED_status, LOW);  // Nyalakan LED saat tag masuk range
    }
  }

  // Reset flag saat tag keluar range
  if (!inRange && tagDetected) {
    tagDetected = false;
    Serial.println("🔴 Tag keluar range\n");
    digitalWrite(LED_status, HIGH);  // Nyalakan LED saat tag masuk range
  }

  delay(50);  // Anti spam loop
}