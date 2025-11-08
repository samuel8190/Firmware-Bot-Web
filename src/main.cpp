// main.cpp — Con sistema de diagnóstico para credenciales WiFi
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

extern "C" uint8_t temprature_sens_read();
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

// PULSADOR DE RESET WIFI - Conectar entre GPIO13 y GND
#define RESET_WIFI_PIN 13

// ---------- CONFIGS ----------
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
const unsigned long READ_INTERVAL = 30000;

unsigned long lastTelegramMillis = 0;
unsigned long telegramInterval = 600000;

unsigned long lastBotCheck = 0;
const unsigned long BOT_POLL_INTERVAL = 5000;

// Variables para el pulsador de reset WiFi
unsigned long lastButtonCheck = 0;
const unsigned long BUTTON_CHECK_INTERVAL = 100;
bool resetWiFiActive = false;
unsigned long resetStartTime = 0;
const unsigned long RESET_HOLD_TIME = 5000; // 5 segundos para reset

// =============================================
// 🆕 NUEVA FUNCIÓN: DIAGNÓSTICO DE CREDENCIALES
// =============================================
void checkWiFiCredentials() {
  Serial.println("\n=== 🔍 VERIFICANDO CREDENCIALES WiFi ===");
  
  if (SPIFFS.exists("/WiFiManager.json")) {
    File file = SPIFFS.open("/WiFiManager.json", "r");
    if (file) {
      String content = file.readString();
      file.close();
      
      Serial.println("✅ Archivo WiFiManager.json EXISTE");
      Serial.println("📄 Contenido:");
      Serial.println(content);
      Serial.println("📏 Tamaño: " + String(content.length()) + " caracteres");
    } else {
      Serial.println("❌ Error abriendo WiFiManager.json");
    }
  } else {
    Serial.println("❌ NO existe WiFiManager.json en SPIFFS");
  }
  
  Serial.println("📶 SSID actual: " + WiFi.SSID());
  Serial.println("🔌 Estado WiFi: " + String(WiFi.status()));
  Serial.println("🌍 IP: " + WiFi.localIP().toString());
  Serial.println("=== 🔍 FIN VERIFICACIÓN ===\n");
}

// =============================================
// 🆕 NUEVA FUNCIÓN: PULSADOR RESET WIFI
// =============================================
void checkResetButton() {
  static bool lastButtonState = HIGH;
  static bool buttonPressed = false;
  
  unsigned long now = millis();
  if (now - lastButtonCheck < BUTTON_CHECK_INTERVAL) return;
  lastButtonCheck = now;
  
  bool currentButtonState = digitalRead(RESET_WIFI_PIN);
  
  // Detectar flanco descendente (botón presionado)
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    buttonPressed = true;
    resetStartTime = now;
    Serial.println("🔄 Pulsador de reset PRESIONADO - Mantén 5 segundos");
  }
  
  // Detectar si se mantiene presionado
  if (buttonPressed && currentButtonState == LOW) {
    unsigned long holdTime = now - resetStartTime;
    
    // Feedback cada segundo
    if (holdTime % 1000 == 0) {
      int secondsLeft = (RESET_HOLD_TIME - holdTime) / 1000;
      Serial.println("Manteniendo pulsador... " + String(secondsLeft) + "s para reset WiFi");
    }
    
    // Si se mantuvo 5 segundos, activar reset
    if (holdTime >= RESET_HOLD_TIME && !resetWiFiActive) {
      resetWiFiActive = true;
      Serial.println("🚨 RESET WIFI ACTIVADO - Borrando credenciales...");
      
      // Verificar credenciales ANTES de borrar
      checkWiFiCredentials();
      
      // Enviar mensaje a Telegram si está conectado
      if (WiFi.status() == WL_CONNECTED) {
        String resetMsg = "🚨 *RESET WIFI ACTIVADO*\n\n";
        resetMsg += "Se presionó el pulsador físico por 5 segundos.\n";
        resetMsg += "Borrando configuración WiFi y reiniciando...";
        bot.sendMessage(CHANNEL_CHAT_ID, resetMsg, "Markdown");
      }
      
      delay(1000);
      wm.resetSettings();
      Serial.println("Credenciales WiFi borradas. Reiniciando...");
      delay(1000);
      ESP.restart();
    }
  }
  
  // Detectar flanco ascendente (botón liberado)
  if (lastButtonState == LOW && currentButtonState == HIGH) {
    buttonPressed = false;
    if (!resetWiFiActive) {
      Serial.println("Pulsador liberado - Reset cancelado");
    }
  }
  
  lastButtonState = currentButtonState;
}

// =============================================
// 🔄 TUS FUNCIONES ORIGINALES (SIN CAMBIOS)
// =============================================
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

void saveToHistory(float temp, float hum) {
  if (!SPIFFS.exists("/history.json")) {
    File f = SPIFFS.open("/history.json", "w");
    if (!f) return;
    f.print("{}");
    f.close();
  }
  
  File f = SPIFFS.open("/history.json", "r");
  if (!f) return;

  DynamicJsonDocument doc(6144);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  String today = getDate();
  JsonArray arr = doc[today];
  if (arr.isNull()) arr = doc.createNestedArray(today);

  if (arr.size() > 50) {
    arr.remove(0);
  }

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
  if (!SPIFFS.exists("/history.json")) { 
    server.send(200, "application/json", "[]"); 
    return; 
  }
  
  File f = SPIFFS.open("/history.json", "r");
  DynamicJsonDocument doc(6144);
  if (deserializeJson(doc, f)) { 
    f.close(); 
    server.send(500,"application/json","{}"); 
    return; 
  }
  f.close();
  
  if (!doc.containsKey(date)) { 
    server.send(200,"application/json","[]"); 
    return; 
  }

  String out; 
  serializeJson(doc[date], out);
  server.send(200, "application/json", out);
}

void handleActuator() {
  if (!server.hasArg("id") || !server.hasArg("state")) {
    server.send(400,"application/json","{\"error\":\"missing id or state\"}");
    return;
  }

  int id = server.arg("id").toInt();
  String state = server.arg("state");
  
  if (id < 1 || id > 4) { 
    server.send(400,"application/json","{\"error\":\"invalid id\"}"); 
    return; 
  }

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
  
  if (id<1 || id>4) { 
    server.send(400,"application/json","{\"error\":\"invalid id\"}"); 
    return; 
  }

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
  if (code > 0) {
    String resp = http.getString();
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

void handleNewMessages(int numNewMessages) {
  Serial.println("Procesando " + String(numNewMessages) + " mensajes Telegram");
  
  for (int i = 0; i < numNewMessages; i++) {
    String text = bot.messages[i].text;
    String chat_id = bot.messages[i].chat_id;

    if (text.length() == 0 || chat_id.length() == 0 || !text.startsWith("/")) continue;

    Serial.println("Comando recibido: " + text);

    if (text == "/menu") {
      String reply = "📋 *Menú de Comandos:*\n\n";
      reply += "📊 /DataSensores - Datos actuales\n";
      reply += "⏱️ /setInterval [seg] - Cambiar intervalo\n";
      reply += "📈 /status - Estado general\n";
      reply += "🖥️ /infoDevices - Info del dispositivo\n";
      reply += "🔐 /resetInfo - Cómo resetear WiFi (Físico)";
      if (bot.sendMessage(chat_id, reply, "Markdown")) {
        Serial.println("Menú enviado OK");
      } else {
        bot.sendMessage(chat_id, reply, "");
      }

    } else if (text == "/DataSensores") {
      String tempStr = isnan(temperature) ? "N/A" : String(temperature, 1) + " °C";
      String humStr  = isnan(humidity) ? "N/A" : String(humidity, 1) + " %";
      String message = "📡 Datos actuales:\n\n";
      message += "🌡️ Temperatura: " + tempStr + "\n";
      message += "💧 Humedad: " + humStr + "\n";
      bot.sendMessage(chat_id, message, "");

    } else if (text == "/APreset") {
      // COMANDO DESHABILITADO - Solo información
      String info = "🔐 *Reset de WiFi - Método Seguro*\n\n";
      info += "Por seguridad, el reset WiFi ahora es *FÍSICO*:\n\n";
      info += "1. 🔘 *Localiza el pulsador* en el ESP32 (GPIO13)\n";
      info += "2. ⏱️ *Mantén pulsado* por *5 segundos*\n";
      info += "3. 🗑️ Se borrarán las credenciales WiFi\n";
      info += "4. 🔄 El ESP32 se reiniciará en modo AP\n";
      info += "5. 📱 Conéctate a: *ESP32_DHT_Setup*\n";
      info += "6. 🌐 Ve a *192.168.4.1* para configurar\n\n";
      info += "✅ *Ventaja:* Nadie puede resetearlo remotamente";
      bot.sendMessage(chat_id, info, "Markdown");

    } else if (text.startsWith("/setInterval ")) {
      String arg = text.substring(13);
      int newInterval = arg.toInt() * 1000;
      if (newInterval > 0) {
        telegramInterval = newInterval;
        bot.sendMessage(chat_id, "⏱️ Intervalo cambiado a " + arg + " segundos.", "");
      } else {
        bot.sendMessage(chat_id, "⚠️ Intervalo inválido.", "");
      }

    } else if (text == "/status") {
      String message = "📈 Estado General:\n";
      message += "⏱️ Intervalo: " + String(telegramInterval/1000) + " s\n";
      message += "📶 WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Conectado ✅" : "Desconectado ❌") + "\n";
      message += "📡 IP: " + WiFi.localIP().toString() + "\n";
      bot.sendMessage(chat_id, message, "");

    } else if (text == "/infoDevices") {
      Serial.println("Procesando comando /infoDevices");
      
      float cpuTemp = (temprature_sens_read() - 32) / 1.8;
      uint64_t chipid = ESP.getEfuseMac();
      String chipIdStr = String((uint32_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
      long rssi = WiFi.RSSI();
      unsigned long uptimeSec = millis() / 1000;
      unsigned int hours = uptimeSec / 3600;
      unsigned int minutes = (uptimeSec % 3600) / 60;
      unsigned int seconds = uptimeSec % 60;

      unsigned long timeLeft = (telegramInterval > (millis() - lastTelegramMillis))
                              ? (telegramInterval - (millis() - lastTelegramMillis)) / 1000
                              : 0;

      // PRIMER INTENTO: Markdown normal (menos estricto)
      String message = "🖥️ *Información del Dispositivo*\n\n";
      message += "💻 *Chip ID:* `" + chipIdStr + "`\n";
      message += "🌡️ *Temp CPU:* " + String(cpuTemp, 1) + " °C\n";
      message += "📶 *RSSI:* " + String(rssi) + " dBm\n";
      message += "⏱️ *Uptime:* " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s\n";
      message += "🔁 *Próximo envío:* en " + String(timeLeft) + " s\n\n";
      message += "📡 *WiFi SSID:* " + WiFi.SSID() + "\n";
      message += "🌍 *IP local:* " + WiFi.localIP().toString() + "\n";
      message += "🔢 *MAC:* " + WiFi.macAddress() + "\n";

      Serial.println("Enviando infoDevices...");
      
      if (bot.sendMessage(chat_id, message, "Markdown")) {
        Serial.println("InfoDevices enviado OK con Markdown");
      } else {
        // SEGUNDO INTENTO: Texto plano (si falla Markdown)
        Serial.println("Falló Markdown, intentando texto plano...");
        
        String plainMessage = "🖥️ Información del Dispositivo\n\n";
        plainMessage += "💻 Chip ID: " + chipIdStr + "\n";
        plainMessage += "🌡️ Temp CPU: " + String(cpuTemp, 1) + " °C\n";
        plainMessage += "📶 RSSI: " + String(rssi) + " dBm\n";
        plainMessage += "⏱️ Uptime: " + String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s\n";
        plainMessage += "🔁 Próximo envío: en " + String(timeLeft) + " s\n\n";
        plainMessage += "📡 WiFi SSID: " + WiFi.SSID() + "\n";
        plainMessage += "🌍 IP local: " + WiFi.localIP().toString() + "\n";
        plainMessage += "🔢 MAC: " + WiFi.macAddress() + "\n";
        
        bot.sendMessage(chat_id, plainMessage, "");
      }

    } else if (text == "/resetInfo") {
      String info = "🔐 *Instrucciones Reset WiFi*\n\n";
      info += "Usa el PULSADOR FÍSICO en GPIO13:\n";
      info += "• Mantén 5 segundos presionado\n";
      info += "• Espera el mensaje de confirmación\n";
      info += "• Se reiniciará en modo configuración";
      bot.sendMessage(chat_id, info, "Markdown");

    } else {
      bot.sendMessage(chat_id, "❓ Comando no reconocido. Usa /menu.", "");
    }
  }
}

// =============================================
// 🔧 SETUP ACTUALIZADO CON DIAGNÓSTICO
// =============================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== 🔧 INICIANDO SETUP ESP32 DHT22 ===");
  delay(1000);
  
  // 🆕 VERIFICACIÓN INICIAL DE CREDENCIALES
  Serial.println("=== 🔍 VERIFICACIÓN INICIAL ===");
  checkWiFiCredentials();
  
  // Configurar pulsador de reset WiFi
  pinMode(RESET_WIFI_PIN, INPUT_PULLUP);
  Serial.println("🔐 Pulsador de reset WiFi configurado en GPIO13");
  
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
  
  // 🆕 WiFiManager con configuración mejorada
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(30);
  Serial.println("🔧 Configurando WiFiManager...");
  
  bool conectado = wm.autoConnect("ESP32_DHT_Setup");
  
  if (conectado) {
    Serial.println("✅ WiFi CONECTADO correctamente");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    MDNS.begin("esp32dht");
    
    // 🆕 VERIFICACIÓN DESPUÉS DE CONEXIÓN
    Serial.println("=== 🔍 VERIFICACIÓN POST-CONEXIÓN ===");
    checkWiFiCredentials();
  } else {
    Serial.println("❌ WiFi NO CONECTADO - Modo AP activo");
  }

  secureClient.setInsecure();
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleSensorData);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/actuator", HTTP_GET, handleActuator);
  server.on("/pwm", HTTP_GET, handlePWM);
  server.onNotFound([](){ 
    if(!handleFileRead(server.uri())) 
      server.send(404,"text/plain","Not Found"); 
  });

  server.begin();
  Serial.println("🌐 Servidor HTTP iniciado");

  if (WiFi.status() == WL_CONNECTED) {
    String startMsg = "⚙️ *Sistema Iniciado: Monitoreo de DHT22*\n\n";
    startMsg += "🔧 Motivo: ⚙ Power On\n";
    startMsg += "🌍 IP: " + WiFi.localIP().toString() + "\n";
    startMsg += "📶 WiFi: " + WiFi.SSID() + "\n";
    startMsg += "🔢 MAC: " + WiFi.macAddress();

    bot.sendMessage(CHANNEL_CHAT_ID, startMsg, "");
  }
  
  Serial.println("=== 🔧 SETUP COMPLETADO ===\n");
}

// =============================================
// 🔄 LOOP ACTUALIZADO
// =============================================
void loop() {
  server.handleClient();
  
  // 🆕 VERIFICAR PULSADOR DE RESET
  checkResetButton();
  
  unsigned long now = millis();

  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime = now;
    float newT = dht.readTemperature();
    float newH = dht.readHumidity();
    
    if (!isnan(newT) && !isnan(newH)) {
      temperature = newT;
      humidity = newH;
      saveToHistory(temperature, humidity);
      
      if (WiFi.status() == WL_CONNECTED) {
        sendToGoogleSheets();
      }
    }
  }

  if (now - lastTelegramMillis >= telegramInterval) {
    lastTelegramMillis = now;
    sendSensorDataToTelegram();
  }

  if (now - lastBotCheck >= BOT_POLL_INTERVAL) {
    lastBotCheck = now;
    int numNew = bot.getUpdates(bot.last_message_received + 1);
    if (numNew > 0) {
      handleNewMessages(numNew);
    }
  }

  // 🆕 VERIFICACIÓN PERIÓDICA (cada 2 minutos)
  static unsigned long lastCredentialCheck = 0;
  if (now - lastCredentialCheck > 120000) {
    lastCredentialCheck = now;
    Serial.println("=== 🔍 VERIFICACIÓN PERIÓDICA CREDENCIALES ===");
    checkWiFiCredentials();
  }

  delay(50);
}