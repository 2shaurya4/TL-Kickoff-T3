
#include "Particle.h"
SYSTEM_MODE(AUTOMATIC);
SYSTEM_THREAD(ENABLED);
SerialLogHandler logHandler(LOG_LEVEL_WARN);

#include "MFRC522.h"
#include "../include/IntervalActor.h"

const int SS_PIN = A0;
const int RST_PIN = A1;

MFRC522 mfrc522(SS_PIN, RST_PIN);  // Create MFRC522 instance.

void setup() {
    Serial.begin(9600);  // Initialize serial communications with the PC
    mfrc522.setSPIConfig();

    mfrc522.PCD_Init();  // Init MFRC522 card
    Serial.println("Scan PICC to see UID and type...");
}

IntervalActor isitworking(1000, 0, []() {
    byte v = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
    Serial.printlnf("MFRC522 version: 0x%02X", v);  // Should be 0x91 or 0x92
});

void loop() {

    // isitworking.act();
    // Look for new cards
    if (!mfrc522.PICC_IsNewCardPresent()) {
        return;
    }

    // Select one of the cards
    if (!mfrc522.PICC_ReadCardSerial()) {
        return;
    }

    // Dump debug info about the card. PICC_HaltA() is automatically called.
    // mfrc522.PICC_DumpToSerial(&(mfrc522.uid));

    String extractedUid = "";
    for (int i = 0; i < mfrc522.uid.size; i++) {
        extractedUid += String(mfrc522.uid.uidByte[i]);
    }
    Serial.println("Found a card: " + extractedUid);
    // Serial.println("Caught something!");
    // Serial.printf("Current UID is probably: %s", mfrc522.uid);
}