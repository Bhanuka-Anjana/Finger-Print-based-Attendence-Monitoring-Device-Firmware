#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>
#include <WebSocketsClient.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>

// Create fuel gauge object
SFE_MAX1704X lipo(MAX1704X_MAX17043);
double cellPercentage = 0;
bool lowBattery = false;

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

volatile int selectedMenuOption = 0;
const int menuControlBtn = 12;
const int menuItemSelectBtn = 13;
// array for menu items
const char *menuItems[] = {"connect server", "reset wifi", "mark attendance"};

// WiFi settings
const char *ssid = "ESP32-AP";
const char *password = "password";
const char *configFile = "/config.json";
const char *websockets_server_host = "192.168.136.135";
const uint16_t websockets_server_port = 8080;

WebServer server(80);
WebSocketsClient webSocket;
bool APStarted = false;
bool wifiConnected = false;
bool webSocketConnected = false;
bool fingerprintSensorisWorking = false;

// Fingerprint sensor settings
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);

// Function prototypes
void taskUpdateBatteryCellData(void *parameter);
void taskWiFiConnection(void *parameter);
void taskDisplayUpdate(void *parameter);
void taskEnableAPMode(void *parameter);
void taskStartWebSocketClient(void *parameter);
void taskMarkAttendance(void *parameter);
void taskRegisterFingerprint(void *parameter);
void menuControlInterrupt();
void menuSelectInterrupt();
bool connectToStoredWiFi();
void connectToWiFi(const char *ssid, const char *password);
void handleRoot();
void handleSave();
String getAvailableNetworks();
void resetWiFiConfig();
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
void sendFingerprintId(uint16_t id, const char *action);

// Task handles
TaskHandle_t taskHandleCellPercentage;
TaskHandle_t taskHandleWiFiConnection;
TaskHandle_t taskHandleEnableAPMode;
TaskHandle_t taskHandleStartWebSocketServer;
TaskHandle_t taskHandleMarkAttendance;

void setup()
{

  // Initialize I2C communication
  Wire.begin();

  // Initialize serial communication
  Serial2.begin(115200);

  // configure the pin modes
  pinMode(menuControlBtn, INPUT_PULLUP);
  pinMode(menuItemSelectBtn, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(menuControlBtn), menuControlInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(menuItemSelectBtn), menuSelectInterrupt, FALLING);

  // Initialize Fuel Gauge
  // lipo.enableDebugging();
  if (lipo.begin() == false)
  {
    // Serial.println("MAX17043 not detected. Freezing...");
    while (1)
      ;
  }

  lipo.quickStart();
  lipo.setThreshold(20);

  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    // Serial.println("An error occurred while mounting SPIFFS...");
    return;
  }

  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    // Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }
  finger.begin(57600);
  finger.verifyPassword();
  
  display.clearDisplay();

  // Create tasks
  xTaskCreate(taskUpdateBatteryCellData, "CellPercentage", 2048, NULL, 2, &taskHandleCellPercentage);
  xTaskCreate(taskWiFiConnection, "WiFiConnection", 4096, NULL, 3, &taskHandleWiFiConnection);
  xTaskCreate(taskDisplayUpdate, "DisplayUpdate", 2048, NULL, 2, NULL);
  // xTaskCreatePinnedToCore(taskEnableAPMode, "EnableAPMode", 4096, NULL, 2, &taskHandleEnableAPMode, CONFIG_ARDUINO_RUNNING_CORE);
}

void loop()
{
  // Not used in FreeRTOS
}

void taskUpdateBatteryCellData(void *parameter)
{
  while (true)
  {
    // Read battery percentage from MAX17043
    lipo.wake();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    cellPercentage = lipo.getSOC();
    lowBattery = lipo.getAlert();
    lipo.sleep();
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

void taskWiFiConnection(void *parameter)
{
  while (true)
  {
    // Check WiFi connection status
    if (WiFi.status() != WL_CONNECTED)
    {
      // Attempt to connect using stored credentials
      wifiConnected = false;
      if (!APStarted)
      {
        File file = SPIFFS.open(configFile, FILE_READ);

        // Parse JSON from file
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, file);

        const char *ssid = doc["ssid"];
        const char *password = doc["password"];

        file.close();

        // Serial.println("Connecting to stored WiFi...");

        WiFi.begin(ssid, password);
      }
    }
    wifiConnected = true;
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

void taskDisplayUpdate(void *parameter)
{
  while (true)
  {
    display.clearDisplay();

    if (!APStarted)
    {
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      if (wifiConnected)
      {
        display.print("Wifi  |");
      }
      else
      {
        display.print("No Wifi|");
      }
      display.print(" Bat:");
      display.print(cellPercentage);
      display.print("%");

      display.setCursor(0, 10);
      for (int i = 0; i < 3; i++)
      {
        if (selectedMenuOption == i)
        {
          display.print(">");
        }
        else
        {
          display.print(" ");
        }
        display.println(menuItems[i]);
      }
    }
    else
    {
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.print(" Bat:");
      display.print(cellPercentage);
      display.print("%");
      display.setCursor(0, 10);
      display.println("Access Point Mode");
      // display ip address
      display.setCursor(0, 20);
      display.println(WiFi.softAPIP().toString());
    }
    display.display();
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void taskEnableAPMode(void *parameter)
{
  APStarted = true;
  wifiConnected = false;
  resetWiFiConfig();
  // Set ESP32 as an access point
  WiFi.softAP(ssid, password);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  // Serial.println("Access Point started");
  while (APStarted)
  {
    server.handleClient();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void handleRoot()
{
  String ssidList = getAvailableNetworks();
  String html = "<html><body><h1 style='color:blue;'>Enter WiFi Credentials</h1>";
  html += "<form action='/save' method='post'>";
  html += "SSID:<input type='text' name='ssid' list='ssids' autocomplete='off'><br>";
  html += "<datalist id='ssids'>" + ssidList + "</datalist>";
  html += "Password:<input type='password' name='password'><br>";
  html += "<input type='submit'></form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave()
{
  String ssid = server.arg("ssid");
  String password = server.arg("password");

  // Create JSON object
  StaticJsonDocument<200> doc;
  doc["ssid"] = ssid;
  doc["password"] = password;

  // Open the file for writing
  File file = SPIFFS.open(configFile, FILE_WRITE);

  // Serialize JSON to file
  if (serializeJson(doc, file) == 0)
  {
    // Serial.println("Failed to write to file");
  }

  file.close();

  server.send(200, "text/html", "<html><body><h1 style='color:green;'>WiFi Credentials Saved Successfully!</h1></body></html>");

  server.close();

  APStarted = false;
  // Serial.println("Access Point stopped");
}

String getAvailableNetworks()
{
  String ssidList;
  int numNetworks = WiFi.scanNetworks();
  for (int i = 0; i < numNetworks; i++)
  {
    ssidList += "<option value='" + WiFi.SSID(i) + "'>";
  }
  return ssidList;
}

void resetWiFiConfig()
{
  // Delete the WiFi config file
  if (SPIFFS.remove(configFile))
  {
    // Serial.println("WiFi config file deleted successfully");
  }
  else
  {
    // Serial.println("Failed to delete WiFi config file");
  }
}

void menuControlInterrupt()
{
  selectedMenuOption--;
  if (selectedMenuOption < 0)
  {
    selectedMenuOption = 2; // Wrap around to last option
  }
}

void taskStartWebSocketClient(void *parameter)
{
  // Serial.println("Starting WebSocket client...");
  webSocket.begin(websockets_server_host, websockets_server_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocketConnected = true;
  menuItems[0] = "disconnect server";

  while (true)
  {
    if (!wifiConnected)
    {
      webSocket.disconnect();
      webSocketConnected = false;
      menuItems[0] = "connect server";
      break;
    }
    webSocket.loop();
  }
}

void menuSelectInterrupt()
{
  switch (selectedMenuOption)
  {
  case 0:
    // Connect to web socket server
    if (webSocketConnected)
    {
      vTaskDelete(taskHandleStartWebSocketServer);
      webSocketConnected = false;
      menuItems[0] = "connect server";
    }
    else
    {
      xTaskCreate(taskStartWebSocketClient, "WebSocketServer", 4096, NULL, 4, &taskHandleStartWebSocketServer);
    }
    break;
  case 1:
    // Enable the APMode task
    xTaskCreate(taskEnableAPMode, "EnableAPMode", 8000, NULL, 4, &taskHandleEnableAPMode);
    break;
  case 2:
    // Mark attendance
    if (fingerprintSensorisWorking)
    {
      vTaskDelete(taskHandleMarkAttendance);
      menuItems[2] = "mark attendance";
      fingerprintSensorisWorking = false;
    }
    else
    {
      xTaskCreate(taskMarkAttendance, "MarkAttendance", 5000, NULL, 3, &taskHandleMarkAttendance);
    }
    break;
  }
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_TEXT:
  {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    const char *action = doc["action"];
    if (strcmp(action, "enroll") == 0)
    {
      int id = doc["id"];
      xTaskCreate(taskRegisterFingerprint, "RegisterFingerprint", 8000, (void *)id, 4, NULL);
    }
    break;
  }

  default:
    break;
  }
}

void taskMarkAttendance(void *parameter)
{
  while (true)
  {
    fingerprintSensorisWorking = true;
    menuItems[2] = "stop attendance";
    int p = finger.getImage();
    if (p == FINGERPRINT_OK)
    {
      p = finger.image2Tz();
      if (p == FINGERPRINT_OK)
      {
        p = finger.fingerFastSearch();
        if (p == FINGERPRINT_OK)
        {
          if (finger.confidence > 50)
          {
            sendFingerprintId(finger.fingerID, "attendance");
          }
        }
      }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void sendFingerprintId(uint16_t id, const char *action)
{
  DynamicJsonDocument doc(1024);
  doc[action] = id;
  String jsonString;
  serializeJson(doc, jsonString);
  webSocket.sendTXT(jsonString);
}

void taskRegisterFingerprint(void *parameter)
{
  int id = (int)parameter;
  while (true)
  {
    fingerprintSensorisWorking = true;
    menuItems[2] = "stop enrollment";
    int p = finger.getImage();
    if (p == FINGERPRINT_OK)
    {
      p = finger.image2Tz();
      if (p == FINGERPRINT_OK)
      {
        p = finger.createModel();
        if (p == FINGERPRINT_OK)
        {
          p = finger.storeModel(id);
          if (p == FINGERPRINT_OK)
          {
            sendFingerprintId(id, "enroll_confirm");
            break;
          }
        }
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}