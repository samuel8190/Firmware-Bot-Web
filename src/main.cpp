// main.cpp — Unificado: Web server + DHT22 + Google Sheets + Telegram bot
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <DHT.h>
#include "time.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <UniversalTelegramBot.h>

#define DHTPIN 4
#define DHTTYPE DHT22

#define ACTUATOR1_PIN 2
#define ACTUATOR2_PIN 5
#define ACTUATOR3_PIN 18
#define ACTUATOR4_PIN 19

#define PWM1_PIN 25
#define PWM2_PIN 26
#define PWM3_PIN 27
#define PWM4_PIN 14

// ---------- CONFIGS (ajusta según necesites) ----------
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbxBWl_eM-3OfGKilMMSzFzckQMyWf6pcnJFAQM4nIr5Tn1trgP1OdNJ5SJmgPSWK2yqIQ/exec";
const char* GOOGLE_KEY = "virus930..";

#define BOT_TOKEN "8069056952:AAGr8KAM27hzvxR-PzEbw_VMMIAOpmeYuyI"
#define CHANNEL_CHAT_ID "@BotdeSamu"

// NTP
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600;
const int daylightOffset_sec = 0;

// ---------- Globals ----------
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
WiFiManager wm;

WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

float temperature = 0.0;
float humidity = 0.0;
bool actuatorState[4] = {false,false,false,false};
int pwmValue[4] = {0,0,0,0};

unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL = 10000;

// Telegram sending interval (separate from sensor read interval)
unsigned long lastTelegramMillis = 0;
unsigned long telegramInterval = 300000; // default 5 minutes

unsigned long lastBotCheck = 0;
const unsigned long BOT_POLL_INTERVAL = 2000; // poll Telegram every 2s

String getDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01";
  char buffer[11];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
  return String(buffer);
}

String getTimeNow() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "00:00:00";
  char buffer[9];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
  return String(buffer);
}

// Save readings to SPIFFS history.json under date array
void saveToHistory(float temp, float hum) {
  if (!SPIFFS.exists("/history.json")) {
    File f = SPIFFS.open("/history.json", "w");
    if (!f) return;
    f.print("{}");
    f.close();
  }
  File f = SPIFFS.open("/history.json", "r");
  if (!f) return;

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  String today = getDate();
  JsonArray arr = doc[today];
  if (arr.isNull()) arr = doc.createNestedArray(today);

  JsonObject entry = arr.createNestedObject();
  entry["time"] = getTimeNow();
  entry["temp"] = temp;
  entry["hum"] = hum;

  File w = SPIFFS.open("/history.json", "w");
  if (!w) return;
  serializeJson(doc, w);
  w.close();
}

bool handleFileRead(String path) {
  if (path.endsWith("/")) path = "/index.html";
  String type = "text/plain";
  if (path.endsWith(".html")) type = "text/html";
  else if (path.endsWith(".css")) type = "text/css";
  else if (path.endsWith(".js")) type = "application/javascript";

  if (SPIFFS.exists(path)) {
    File f = SPIFFS.open(path, "r");
    server.streamFile(f, type);
    f.close();
    return true;
  }
  return false;
}

void handleRoot() {
  if (!handleFileRead("/index.html"))
    server.send(404, "text/plain", "index.html no encontrado");
}

void handleSensorData() {
  StaticJsonDocument<300> doc;
  doc["temp"] = temperature;
  doc["hum"] = humidity;
  JsonArray a = doc.createNestedArray("actuators");
  for (int i=0;i<4;i++) a.add(actuatorState[i]);
  JsonArray p = doc.createNestedArray("pwm");
  for (int i=0;i<4;i++) p.add(pwmValue[i]);
  doc["ts"] = millis();

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleHistory() {
  if (!server.hasArg("date")) {
    server.send(400, "application/json", "{\"error\":\"missing date\"}");
    return;
  }
  String date = server.arg("date");
  if (!SPIFFS.exists("/history.json")) { server.send(200, "application/json", "[]"); return; }
  File f = SPIFFS.open("/history.json", "r");
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, f)) { f.close(); server.send(500,"application/json","{}"); return; }
  f.close();
  if (!doc.containsKey(date)) { server.send(200,"application/json","[]"); return; }

  String out; serializeJson(doc[date], out);
  server.send(200, "application/json", out);
}

void handleActuator() {
  if (!server.hasArg("id") || !server.hasArg("state")) {
    server.send(400,"application/json","{\"error\":\"missing id or state\"}");
    return;
  }

  int id = server.arg("id").toInt();
  String state = server.arg("state");
  if (id < 1 || id > 4) { server.send(400,"application/json","{\"error\":\"invalid id\"}"); return; }

  int pin = (id==1?ACTUATOR1_PIN:id==2?ACTUATOR2_PIN:id==3?ACTUATOR3_PIN:ACTUATOR4_PIN);
  int idx = id-1;

  if (state == "on") {
    digitalWrite(pin, HIGH);
    actuatorState[idx] = true;
  } else if (state == "off") {
    digitalWrite(pin, LOW);
    actuatorState[idx] = false;
  } else {
    server.send(400,"application/json","{\"error\":\"invalid state\"}");
    return;
  }

  StaticJsonDocument<100> doc;
  doc["status"] = "OK";
  doc["id"] = id;
  doc["actuator"] = actuatorState[idx];
  String j; serializeJson(doc, j);
  server.send(200,"application/json",j);
}

void handlePWM() {
  if (!server.hasArg("id") || !server.hasArg("value")) {
    server.send(400,"application/json","{\"error\":\"missing id or value\"}");
    return;
  }
  int id = server.arg("id").toInt();
  int value = server.arg("value").toInt();
  if (id<1 || id>4) { server.send(400,"application/json","{\"error\":\"invalid id\"}"); return; }

  value = constrain(value,0,255);
  int idx = id-1;
  pwmValue[idx] = map(value,0,255,0,100);
  ledcWrite(idx,value);

  StaticJsonDocument<100> doc;
  doc["status"]="OK";
  doc["id"]=id;
  doc["value"]=pwmValue[idx];
  String j; serializeJson(doc,j);
  server.send(200,"application/json",j);
}

void sendToGoogleSheets() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;

  String payload = "key=" + String(GOOGLE_KEY) +
                   "&date=" + getDate() +
                   "&time=" + getTimeNow() +
                   "&temp=" + String(temperature, 1) +
                   "&hum=" + String(humidity, 1);

  http.begin(secureClient, GOOGLE_SCRIPT_URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int code = http.POST(payload);
  Serial.printf("Sheets HTTP: %d\n", code);
  if (code > 0) {
    String resp = http.getString();
    Serial.println("Respuesta Sheets: " + resp);
  }
  http.end();
}

void sendSensorDataToTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;
  String tempStr = isnan(temperature) ? "N/A" : String(temperature, 1) + " °C";
  String humStr  = isnan(humidity) ? "N/A" : String(humidity, 1) + " %";

  String message = "📡 Lectura DHT22\n";
  message += getDate() + " " + getTimeNow() + "\n\n";
  message += "🌡️ Temperatura: " + tempStr + "\n";
  message += "💧 Humedad: " + humStr + "\n";

  bot.sendMessage(CHANNEL_CHAT_ID, message, "");
}

// --- Telegram message handling (copiado/adaptado del bot original) ---
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String text = bot.messages[i].text;
    String chat_id = bot.messages[i].chat_id;

    if (text.length() == 0 || chat_id.length() == 0 || !text.startsWith("/")) continue;

    if (text == "/menu") {
      String reply = "📋 *Menú de Comandos:*\n\n";
      reply += "📊 /DataSensores - Datos actuales\n";
      reply += "🧹 /APreset - Borrar WiFi y reiniciar\n";
      reply += "⏱️ /setInterval [seg] - Cambiar intervalo de Telegram\n";
      reply += "📈 /status - Estado general\n";
      reply += "🔁 /reset - Reiniciar ESP32\n";
      reply += "🖥️ /infoDevices - Info del dispositivo\n";
      bot.sendMessage(chat_id, reply, "Markdown");

    } else if (text == "/DataSensores") {
      // Responder con lectura inmediata
      String tempStr = isnan(temperature) ? "N/A" : String(temperature, 1) + " °C";
      String humStr  = isnan(humidity) ? "N/A" : String(humidity, 1) + " %";
      String message = "📡 Datos actuales:\n\n";
      message += "🌡️ Temperatura: " + tempStr + "\n";
      message += "💧 Humedad: " + humStr + "\n";
      bot.sendMessage(chat_id, message, "");

    } else if (text == "/APreset") {
      wm.resetSettings();
      bot.sendMessage(chat_id, "🧹 Configuración WiFi borrada. Reiniciando...", "");
      delay(1000);
      ESP.restart();

    } else if (text.startsWith("/setInterval ")) {
      String arg = text.substring(13);
      int newInterval = arg.toInt() * 1000;
      if (newInterval > 0) {
        telegramInterval = newInterval;
        bot.sendMessage(chat_id, "⏱️ Intervalo de Telegram cambiado a " + arg + " segundos.", "");
      } else {
        bot.sendMessage(chat_id, "⚠️ Intervalo inválido. Usa un número positivo.", "");
      }

    } else if (text == "/status") {
      String message = "📈 Estado General:\n";
      message += "⏱️ Intervalo Telegram: " + String(telegramInterval/1000) + " s\n";
      message += "📶 WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Conectado ✅" : "Desconectado ❌") + "\n";
      message += "📡 IP: " + WiFi.localIP().toString() + "\n";
      bot.sendMessage(chat_id, message, "");

    } else if (text == "/reset") {
      bot.sendMessage(chat_id, "Reiniciando...", "");
      delay(1000);
      ESP.restart();

    } else if (text == "/infoDevices") {
      String message = "Info del dispositivo:\n";
      message += "MAC: " + WiFi.macAddress() + "\n";
      message += "SSID: " + WiFi.SSID() + "\n";
      message += "IP: " + WiFi.localIP().toString() + "\n";
      bot.sendMessage(chat_id, message, "");

    } else {
      bot.sendMessage(chat_id, "Comando no reconocido. Usa /menu", "");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  SPIFFS.begin(true);
  dht.begin();

  pinMode(ACTUATOR1_PIN, OUTPUT); digitalWrite(ACTUATOR1_PIN, LOW);
  pinMode(ACTUATOR2_PIN, OUTPUT); digitalWrite(ACTUATOR2_PIN, LOW);
  pinMode(ACTUATOR3_PIN, OUTPUT); digitalWrite(ACTUATOR3_PIN, LOW);
  pinMode(ACTUATOR4_PIN, OUTPUT); digitalWrite(ACTUATOR4_PIN, LOW);

  ledcSetup(0,5000,8); ledcAttachPin(PWM1_PIN,0);
  ledcSetup(1,5000,8); ledcAttachPin(PWM2_PIN,1);
  ledcSetup(2,5000,8); ledcAttachPin(PWM3_PIN,2);
  ledcSetup(3,5000,8); ledcAttachPin(PWM4_PIN,3);

  WiFi.mode(WIFI_STA);
  wm.autoConnect("ESP32_DHT_Setup");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  MDNS.begin("esp32dht");

  secureClient.setInsecure(); // para conexiones HTTPS sin validar certificado

  // Rutas HTTP
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleSensorData);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/actuator", HTTP_GET, handleActuator);
  server.on("/pwm", HTTP_GET, handlePWM);
  server.onNotFound([](){ if(!handleFileRead(server.uri())) server.send(404,"text/plain","Not Found"); });

  server.begin();

  Serial.println("Servidor iniciado. IP: " + WiFi.localIP().toString());

  // Enviar mensaje de arranque por Telegram (intento, sin bloquear)
  String bootMsg = "⚠️ Sistema iniciado en ESP32\nIP: " + WiFi.localIP().toString();
  bot.sendMessage(CHANNEL_CHAT_ID, bootMsg, "");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // Leer sensor cada READ_INTERVAL
  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime = now;
    float newT = dht.readTemperature();
    float newH = dht.readHumidity();
    if (!isnan(newT) && !isnan(newH)) {
      temperature = newT;
      humidity = newH;
      saveToHistory(temperature, humidity);
      sendToGoogleSheets();
    } else {
      Serial.println("DHT lectura NAN");
    }
  }

  // Enviar a Telegram cada telegramInterval (no bloqueante)
  if (now - lastTelegramMillis >= telegramInterval) {
    lastTelegramMillis = now;
    sendSensorDataToTelegram();
  }

  // Revisar mensajes de Telegram periódicamente
  if (now - lastBotCheck >= BOT_POLL_INTERVAL) {
    lastBotCheck = now;
    int numNew = bot.getUpdates(bot.last_message_received + 1);
    if (numNew > 0) handleNewMessages(numNew);
  }

  delay(20); // pequeña pausa para estabilidad
}
