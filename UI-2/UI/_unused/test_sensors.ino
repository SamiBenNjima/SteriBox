#include <Wire.h>
#include <RTClib.h>
#include <SPI.h>
#include <SD.h>

// --- Librairies futures (commentées pour éviter les erreurs de compilation) ---
// #include <DHT.h>       // Pour le futur DHT22 (déporté sur I2C)
// #include <PCF8574.h>   // Pour le futur extenseur PCF8574
// #include <Ch376msc.h>  // Pour le futur lecteur USB CH376S

// --- Configuration de la carte MicroSD interne du CrowPanel ---
// Broche Chip Select pour le lecteur de carte SD interne
#define SD_CS_PIN 10 

// --- Configuration du RTC ---
// Connecté sur les broches I2C 19 (SDA) et 20 (SCL)
RTC_DS1307 rtc;

// --- Configuration de l'extenseur I2C (Futur) ---
// Adresse 0x20 (Commutateurs rouges tous sur OFF)
// PCF8574 pcf8574(0x20); 

// --- Configuration du DHT22 sur l'extenseur I2C (Futur) ---
// Le DHT22 sera connecté plus tard sur une broche de l'extenseur PCF8574 
// ou géré de manière déportée sur le bus I2C.

// --- Configuration du CH376S (Futur) ---
// Note : Le module CH376S utilisera le port Série (Serial) pour communiquer.
// Ch376msc flashDrive(Serial); 

// --- Variables de cadencement (Évite l'usage de delay) ---
unsigned long chronoRTC = 0;
unsigned long chronoSD = 0;

void setup() {
  // Initialisation de la communication série pour le débogage et l'affichage des valeurs
  Serial.begin(115200);
  delay(1000); // Laisse le temps à la console série de s'ouvrir
  Serial.println("=== Test SteriBox - Capteurs RTC + Carte SD ===");

  // 1. Initialisation du Bus I2C Principal (Partagé : RTC DS1307)
  // Les broches 19 et 20 sont les broches I2C natives de votre CrowPanel
  Wire.begin(19, 20); 
  Serial.println("Bus I2C initialisé sur les broches 19 (SDA) et 20 (SCL).");

  // 2. Initialisation de votre écran graphique (Futur / LVGL)
  // tft.begin();
  // tft.setRotation(1);

  // 3. Initialisation du module Horloge RTC (Connecté physiquement)
  if (!rtc.begin()) {
    Serial.println("ERREUR : RTC DS1307 non détecté ! Vérifiez son alimentation (5V/3.3V) et ses liaisons SDA/SCL.");
  } else {
    Serial.println("RTC DS1307 détecté avec succès !");
    if (!rtc.isrunning()) {
      Serial.println("Le RTC ne tournait pas. Réglage de l'heure avec celle du PC de compilation...");
      // Si le RTC a perdu l'heure (première utilisation), on applique l'heure du PC
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    } else {
      Serial.println("Le RTC tourne déjà.");
    }
  }

  // 4. Initialisation de la carte MicroSD interne
  // Configuration des broches SPI matérielles du CrowPanel : CLK=12, MISO=13, MOSI=11, CS=10
  SPI.begin(12, 13, 11, 10);
  
  Serial.print("Initialisation de la carte SD...");
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("ÉCHEC (carte absente ou mal formatée en FAT32).");
  } else {
    Serial.println("Carte SD prête et initialisée avec succès !");
    // Écriture d'une ligne d'en-tête pour marquer le redémarrage
    enregistrerLogSD("Démarrage du système SteriBox - Log démarré.");
  }

  // 5. Initialisation du PCF8574 (Futur - commentée)
  /* 
  pcf8574.pinMode(P0, OUTPUT, HIGH); // Relais 1 (Initialisé à l'état HAUT/Éteint)
  pcf8574.pinMode(P1, OUTPUT, HIGH); // Relais 2
  pcf8574.pinMode(P2, OUTPUT, HIGH); // Relais 3
  pcf8574.pinMode(P3, INPUT_PULLUP); // Bouton poussoir
  pcf8574.begin();
  Serial.println("PCF8574 configuré.");
  */

  // 6. Initialisation du lecteur USB CH376S (Futur - commentée)
  /*
  flashDrive.begin();
  Serial.println("CH376S configuré.");
  */
}

void loop() {
  // =================================================================
  // 🔄 TÂCHE 1 : Gestion de l'écran tactile (Futur / LVGL)
  // =================================================================
  // Si vous utilisez LVGL, appelez votre gestionnaire ici :
  // lv_timer_handler(); 

  // =================================================================
  // 🔘 TÂCHE 2 : Lecture instantanée du bouton (Futur - commentée)
  // =================================================================
  /*
  int etatBouton = pcf8574.digitalRead(P3);
  if (etatBouton == LOW) { 
    // Le bouton est pressé : On active le relais 1 (LOW ou HIGH selon votre carte relais)
    pcf8574.digitalWrite(P0, LOW); 
  } else {
    // Le bouton est relâché : On éteint le relais 1
    pcf8574.digitalWrite(P0, HIGH); 
  }
  */

  // =================================================================
  // ⏰ TÂCHE 3 : Lecture de l'heure du RTC (Seulement toutes les secondes)
  // =================================================================
  if (millis() - chronoRTC >= 1000) {
    chronoRTC = millis();
    
    // On vérifie que le RTC est accessible
    if (rtc.begin()) {
      DateTime maintenant = rtc.now();

      // Formatage de la date et de l'heure pour l'affichage série
      char cacheHeure[32];
      sprintf(cacheHeure, "%02d/%02d/%04d %02d:%02d:%02d", 
              maintenant.day(), maintenant.month(), maintenant.year(),
              maintenant.hour(), maintenant.minute(), maintenant.second());
      
      Serial.print("[RTC] ");
      Serial.println(cacheHeure);
    }
  }

  // =================================================================
  // 💾 TÂCHE 4 : Enregistrement sur la carte SD (Toutes les 10 secondes)
  // =================================================================
  if (millis() - chronoSD >= 10000) {
    chronoSD = millis();
    enregistrerLogSD("Système OK - En attente de l'extenseur I2C.");
  }

  // =================================================================
  // 💾 TÂCHE 5 : Surveillance asynchrone de la clé USB (Futur - commentée)
  // =================================================================
  /*
  flashDrive.checkDrive(); 
  if (flashDrive.driveStatus() == STATUS_DISK_MOUNTED) {
    // Une clé USB est insérée et prête
    // Vous pouvez ici ouvrir un fichier pour y inscrire l'historique
  }
  */
}

// Fonction pratique pour enregistrer un message dans le fichier log.txt sur la carte SD
void enregistrerLogSD(const char * message) {
  // Ouvre le fichier "log.txt" en mode écriture (crée le fichier s'il n'existe pas)
  File monFichier = SD.open("/log.txt", FILE_WRITE);
  
  if (monFichier) {
    Serial.print("Écriture sur la carte SD...");
    
    // Si le RTC fonctionne, on préfixe le log avec l'heure réelle
    if (rtc.isrunning()) {
      DateTime maintenant = rtc.now();
      char cacheHeure[32];
      sprintf(cacheHeure, "[%02d/%02d/%04d %02d:%02d:%02d] ", 
              maintenant.day(), maintenant.month(), maintenant.year(),
              maintenant.hour(), maintenant.minute(), maintenant.second());
      monFichier.print(cacheHeure);
    } else {
      monFichier.print("[Pas de RTC] ");
    }
    
    // Écrit le message personnalisé
    monFichier.println(message);
    
    // Fermeture obligatoire du fichier pour valider l'enregistrement physique
    monFichier.close(); 
    Serial.println("Terminé.");
  } else {
    Serial.println("Erreur lors de l'ouverture du fichier log.txt !");
  }
}
