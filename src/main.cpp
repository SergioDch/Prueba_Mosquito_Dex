/*
 * =====================================================================
 *  PRUEBA MQTT sobre WebSocket - ESP32 -> ws://mqqt.diformosa.com/mqtt
 * =====================================================================
 */
#include <Arduino.h>
#include <WiFi.h>
#include "mqtt_client.h"
#include <DHT.h>
#include "secrets.h"
#include "ca_certs.h"

// =====================================================================
//  SENSOR DHT21 - pin de datos en GPIO18, con pull-up
// =====================================================================
#define PIN_DHT 18
DHT dht(PIN_DHT, DHT21);
float ultimaTemp = NAN;
float ultimaHum = NAN;

// =====================================================================
//  REDES WIFI CONOCIDAS Y CREDENCIALES MQTT -> ver include/secrets.h
//  (no se sube a git; copiar secrets.h.example y completar)
// =====================================================================
size_t wifiIdx = 0;

// =====================================================================
//  BROKER MQTT (WebSocket seguro - HiveMQ Cloud propio)
// =====================================================================
const char* MQTT_URI = "wss://5dab8a9752864256b9b112a6465de82a.s1.eu.hivemq.cloud:8884/mqtt";

esp_mqtt_client_handle_t mqttClient = nullptr;
bool mqttIniciado = false;
volatile bool mqttConectado = false;

char mqttClientId[32];
char mqttTopic[48];

unsigned long tUltPublish = 0;
const unsigned long PUBLISH_CADA_MS = 5000;

// =====================================================================
//  WIFI - conecta probando la lista de redes por turnos
// =====================================================================
void conectarWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  const WifiCred& c = WIFI_LIST[wifiIdx];
  Serial.printf("Conectando a WiFi: %s\n", c.ssid);
  WiFi.begin(c.ssid, c.pass);
  wifiIdx = (wifiIdx + 1) % WIFI_COUNT;

  unsigned long tInicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - tInicio < 8000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi OK. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nNo conecto, se probara la siguiente red.");
  }
}

// =====================================================================
//  MQTT - eventos del cliente ESP-IDF
// =====================================================================
static void mqttEventHandler(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;
  switch ((esp_mqtt_event_id_t)eventId) {
    case MQTT_EVENT_CONNECTED:
      mqttConectado = true;
      Serial.println("MQTT conectado (WebSocket).");
      break;
    case MQTT_EVENT_DISCONNECTED:
      mqttConectado = false;
      Serial.println("MQTT desconectado.");
      break;
    case MQTT_EVENT_ERROR:
      Serial.println("MQTT error de conexion/transporte.");
      break;
    case MQTT_EVENT_PUBLISHED:
      Serial.printf("MQTT: broker confirmo el publish (msg_id=%d)\n", event->msg_id);
      break;
    default:
      break;
  }
}

void iniciarMqtt() {
  esp_mqtt_client_config_t cfg = {};
  cfg.uri = MQTT_URI;
  cfg.client_id = mqttClientId;
  if (MQTT_USER) cfg.username = MQTT_USER;
  if (MQTT_PASS) cfg.password = MQTT_PASS;
  cfg.cert_pem = ISRG_ROOT_X1;

  mqttClient = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY, mqttEventHandler, NULL);
  esp_mqtt_client_start(mqttClient);
  mqttIniciado = true;
}

// =====================================================================
//  LECTURA DEL DHT21 Y PUBLICACION
// =====================================================================
void leerYPublicar() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  // El DHT a veces devuelve NAN en una lectura suelta (ruido electrico,
  // se lo llamo antes de que este listo, etc.); si pasa, se mantiene la
  // ultima lectura buena en vez de mandar un dato basura.
  if (!isnan(t)) ultimaTemp = t;
  if (!isnan(h)) ultimaHum = h;

  if (isnan(ultimaTemp) || isnan(ultimaHum)) {
    Serial.println("DHT21 sin lectura valida todavia (revisar cableado/pull-up en GPIO18).");
    return;
  }

  char payload[128];
  int len = snprintf(payload, sizeof(payload),
                      "{\"temp_c\":%.1f,\"hum\":%.1f,\"uptime_s\":%lu}",
                      ultimaTemp, ultimaHum, millis() / 1000);

  int msgId = esp_mqtt_client_publish(mqttClient, mqttTopic, payload, len, 1, 0);
  Serial.printf("Publicado en %s -> %s (msg_id=%d, esperando confirmacion del broker...)\n", mqttTopic, payload, msgId);
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_DHT, INPUT_PULLUP);   // pull-up por software, ademas del pull-up fisico en la linea de datos
  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  uint64_t mac = ESP.getEfuseMac();
  snprintf(mqttClientId, sizeof(mqttClientId), "esp32-%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  snprintf(mqttTopic, sizeof(mqttTopic), "mosquito1/%04X%08X/sensores", (uint16_t)(mac >> 32), (uint32_t)mac);
  Serial.printf("Client ID: %s\n", mqttClientId);
  Serial.printf("Topic:     %s\n", mqttTopic);
  Serial.printf("Broker:    %s\n", MQTT_URI);

  randomSeed(esp_random());
}

void loop() {
  conectarWifi();

  if (WiFi.status() == WL_CONNECTED && !mqttIniciado) {
    iniciarMqtt();   // se inicia una sola vez; el cliente ESP-IDF reconecta solo
  }

  if (mqttConectado && millis() - tUltPublish >= PUBLISH_CADA_MS) {
    tUltPublish = millis();
    leerYPublicar();
  }
}
