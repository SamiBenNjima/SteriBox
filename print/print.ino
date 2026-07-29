#include <WiFi.h>

// ================================================================
// CONFIGURATION WI-FI DIRECT & MATÉRIEL
// ================================================================
const char* wfidirect_ssid = "DIRECT-D2-EPSON-928475"; 
const char* wfidirect_pass = "48364164";                    
const int printer_port     = 9100;

const int TRIGGER_PIN      = 34; // Broche de déclenchement (GPIO 34)

int lastState = LOW;             // Pour détecter le changement d'état (front montant)
IPAddress printer_ip;
WiFiClient client;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Configuration de la broche 34 en entrée
  pinMode(TRIGGER_PIN, INPUT);

  Serial.println("\n==========================================");
  Serial.println("  ESP32 - Impression sur Signal HAUT ");
  Serial.println("==========================================");
  Serial.print("Connexion au Wi-Fi Direct : ");
  Serial.println(wfidirect_ssid);

  // Connexion Wi-Fi Direct
  WiFi.mode(WIFI_STA);
  WiFi.begin(wfidirect_ssid, wfidirect_pass);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n[OK] Connecté au Wi-Fi Direct !");
  
  // Récupération automatique de l'IP de l'imprimante
  printer_ip = WiFi.gatewayIP();
  Serial.print("IP de l'imprimante détectée : ");
  Serial.println(printer_ip);

  Serial.println("\n-------------------------------------------------");
  Serial.println(">>> En attente d'un signal HAUT (3.3V) sur la broche 34...");
  Serial.println("-------------------------------------------------");
}

void loop() {
  // Lecture de l'état actuel de la broche 34
  int currentState = digitalRead(TRIGGER_PIN);

  // Détection du Front Montant : Passage de LOW (0V) à HIGH (3.3V)
  if (currentState == HIGH && lastState == LOW) {
    Serial.println("\n[!] SIGNAL HAUT DÉTECTÉ sur la broche 34 !");

    // Vérifier si le Wi-Fi est toujours connecté
    if (WiFi.status() == WL_CONNECTED) {
      imprimerTest(printer_ip);
    } else {
      Serial.println("[ERREUR] Wi-Fi déconnecté. Reconnexion en cours...");
      WiFi.reconnect();
    }

    // Anti-rebond et pause de sécurité (2 secondes) pour éviter les fausses impressions
    delay(2000); 
  }

  lastState = currentState; // Sauvegarde de l'état pour la prochaine boucle
  delay(50);                // Petite pause de stabilité
}

// Fonction qui envoie la commande d'impression
void imprimerTest(IPAddress ip) {
  Serial.println("Ouverture de la connexion vers l'imprimante...");

  if (client.connect(ip, printer_port)) {
    Serial.println("[OK] Impression en cours...");

    // Contenu imprimé
    client.println("==========================================");
    client.println("    IMPRESSION DECLENCHEE PAR CAPTEUR     ");
    client.println("==========================================");
    client.println("Declencheur : Signal HAUT (3.3V) sur PIN 34");
    client.println("Imprimante  : Epson L3250");
    client.println("------------------------------------------");
    client.println("L'impression a ete executee avec succes !");
    client.println("==========================================");
    client.println("\n\n");

    // Code ASCII Form Feed (0x0C) -> Éjection automatique de la page
    client.print("\x0C");

    client.stop();
    Serial.println("[SUCCÈS] Page éjectée. En attente du prochain signal sur PIN 34...\n");
  } else {
    Serial.println("[ERREUR] Échec de connexion au port 9100 de l'imprimante.");
  }
}
