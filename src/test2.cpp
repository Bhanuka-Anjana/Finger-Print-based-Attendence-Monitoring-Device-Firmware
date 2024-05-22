#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
#include <Adafruit_Fingerprint.h>
#include <FreeRTOS.h>

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// WiFi settings
const char* ssid = "YourWiFiSSID";
const char* password = "YourWiFiPassword";

// WebSocket settings
const char* serverAddress = "your.server.address";
const uint16_t serverPort = 8080;
WebSocketsClient webSocket;

// Fuel gauge
MAX17043 batteryMonitor;

// Fingerprint sensor
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// Buttons
#define BUTTON_PIN1 34  // Button to toggle menu
#define BUTTON_PIN2 35  // Button to select menu item

// Task handles
TaskHandle_t taskHandleBattery;
TaskHandle_t taskHandleWiFi;
TaskHandle_t taskHandleDisplay;
TaskHandle_t taskHandleFingerprint;

// Global variables
int currentMenu = 0;
const char* menuItems[] = {"Connect to Server", "Reset WiFi"};

// Function prototypes
void taskBatteryMonitor(void* parameter);
void taskWiFiHandler(void* parameter);
void taskDisplayHandler(void* parameter);
void taskFingerprintHandler(void* parameter);
void connectToWiFi();
void connectToWebSocket();
void resetWiFi();
void goToAPMode();
void handleWebSocketMessage(WStype_t type, uint8_t* payload, size_t length);

void setup() {
  Serial.begin(115200);

  // Initialize I2C communication
  Wire.begin();

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An error occurred while mounting SPIFFS...");
    return;
  }

  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  display.display();
  delay(2000);
  display.clearDisplay();

  // Initialize MAX17043 fuel gauge
  batteryMonitor.begin();

  // Initialize fingerprint sensor
  finger.begin(57600);

  // Initialize buttons
  pinMode(BUTTON_PIN1, INPUT_PULLUP);
  pinMode(BUTTON_PIN2, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN1), toggleMenu, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN2), selectMenuItem, FALLING);

  // Create tasks
  xTaskCreatePinnedToCore(taskBatteryMonitor, "BatteryMonitor", 10000, NULL, 1, &taskHandleBattery, 1);
  xTaskCreatePinnedToCore(taskWiFiHandler, "WiFiHandler", 10000, NULL, 2, &taskHandleWiFi, 1);
  xTaskCreatePinnedToCore(taskDisplayHandler, "DisplayHandler", 10000, NULL, 3, &taskHandleDisplay, 1);
  xTaskCreatePinnedToCore(taskFingerprintHandler, "FingerprintHandler", 10000, NULL, 4, &taskHandleFingerprint, 1);
}

void loop() {
  // Not used in FreeRTOS
}

void taskBatteryMonitor(void* parameter) {
  while (true) {
    // Read battery percentage from MAX17043
    float batteryPercentage = batteryMonitor.getSOC();

    // Display battery percentage on OLED
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Battery:");
    display.setCursor(0, 10);
    display.print(batteryPercentage);
    display.print("%");
    display.display();

    // Check if battery percentage is low
    if (batteryPercentage < 20) {
      // Display warning message
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 20);
      display.println("Low battery!");
      display.display();
    }

    vTaskDelay(1000);
  }
}

void taskWiFiHandler(void* parameter) {
  while (true) {
    // Check WiFi connection status
    if (WiFi.status() != WL_CONNECTED) {
      connectToWiFi();
    } else {
      // Maintain WebSocket connection
      webSocket.loop();
    }

    vTaskDelay(1000);
  }
}

void taskDisplayHandler(void* parameter) {
  while (true) {
    // Display current menu
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print("Menu:");
    display.setCursor(0, 10);
    display.print(menuItems[currentMenu]);
    display.display();

    vTaskDelay(100);
  }
}

void taskFingerprintHandler(void* parameter) {
  while (true) {
    // Handle fingerprint sensor operations
    // This is a placeholder for actual fingerprint handling logic

    vTaskDelay(1000);
  }
}

void connectToWiFi() {
  // Attempt to connect using stored credentials
  File file = SPIFFS.open("/wifi_config.json", FILE_READ);
  if (!file) {
    Serial.println("Failed to open file for reading, going to AP mode");
    goToAPMode();
    return;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println("Failed to read from file, going to AP mode");
    goToAPMode();
    return;
  }

  const char* storedSSID = doc["ssid"];
  const char* storedPassword = doc["password"];
  file.close();

  Serial.println("Connecting to WiFi...");
  WiFi.begin(storedSSID, storedPassword);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to WiFi");
    connectToWebSocket();
  } else {
    Serial.println("Failed to connect to WiFi, going to AP mode");
    goToAPMode();
  }
}

void connectToWebSocket() {
  webSocket.begin(serverAddress, serverPort, "/");
  webSocket.onEvent(handleWebSocketMessage);
}

void resetWiFi() {
  SPIFFS.remove("/wifi_config.json");
  goToAPMode();
}

void goToAPMode() {
  WiFi.softAP("ESP32-AP");

  // Serve a web page to enter WiFi credentials
  AsyncWebServer server(80);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });
  server.on("/get", HTTP_GET, [](AsyncWebServerRequest* request) {
    String ssid = request->getParam("ssid")->value();
    String password = request->getParam("password")->value();

    DynamicJsonDocument doc(1024);
    doc["ssid"] = ssid;
    doc["password"] = password;

    File file = SPIFFS.open("/wifi_config.json", FILE_WRITE);
    serializeJson(doc, file);
    file.close();

    request->send(200, "text/plain", "Credentials Saved, Rebooting...");
    delay(1000);
    ESP.restart();
  });
  server.begin();
}

void handleWebSocketMessage(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_TEXT) {
    // Handle incoming WebSocket messages
    String message = String((char*)payload);
    Serial.println("WebSocket message received: " + message);
  }
}

void toggleMenu() {
  currentMenu = (currentMenu + 1) % 2;
}

void selectMenuItem() {
  switch (currentMenu) {
    case 0:
      connectToWebSocket();
      break;
    case 1:
      resetWiFi();
      break;
  }
}

String processor(const String& var) {
  return String();
}
