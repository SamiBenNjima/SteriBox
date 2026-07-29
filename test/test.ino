#include <Wire.h>
#include "RTClib.h"

RTC_DS3231 rtc;

// Broches I2C du port blanc de la carte CrowPanel 5.0" (ESP32-S3)
#define CROWPANEL_SDA 19
#define CROWPANEL_SCL 20

void setup() {
  Serial.begin(115200);
  delay(1000); // Laisse le temps au moniteur série de s'ouvrir

  Serial.println("\n=============================================");
  Serial.println("=== Test RTC DS3231 - CrowPanel ESP32-S3 ===");
  Serial.println("=============================================");

  // Démarre l'I2C sur les broches physiques IO19 (SDA) et IO20 (SCL) du CrowPanel
  Wire.begin(CROWPANEL_SDA, CROWPANEL_SCL);
  Wire.setClock(100000);

  if (!rtc.begin()) {
    Serial.println("❌ Module RTC non détecté ! Vérifiez le câblage sur le port blanc I2C (SDA=19, SCL=20, 3.3V, GND).");
    while (1) {
      delay(1000); // Bloque proprement en cas d'erreur
    }
  }

  // ⚠️ Décommente cette ligne UNE SEULE FOIS pour régler l'heure sur celle du PC,
  // puis re-commente-la et re-téléverse.
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  // Alternative : régler manuellement (année, mois, jour, heure, minute, seconde)
  // rtc.adjust(DateTime(2026, 7, 28, 12, 30, 0));

  Serial.println("✅ RTC DS3231 détecté et prêt !");
}

void loop() {
  DateTime now = rtc.now();

  // Affichage formaté (JJ/MM/AAAA  HH:MM:SS)
  Serial.print("📅 ");
  if (now.day() < 10) Serial.print('0');
  Serial.print(now.day());
  Serial.print('/');
  if (now.month() < 10) Serial.print('0');
  Serial.print(now.month());
  Serial.print('/');
  Serial.print(now.year());

  Serial.print("  🕐 ");
  if (now.hour() < 10) Serial.print('0');
  Serial.print(now.hour());
  Serial.print(':');
  if (now.minute() < 10) Serial.print('0');
  Serial.print(now.minute());
  Serial.print(':');
  if (now.second() < 10) Serial.print('0');
  Serial.print(now.second());

  Serial.println();
  delay(1000);  // Rafraîchissement toutes les secondes
}
