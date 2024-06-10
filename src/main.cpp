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
const char *menuItems[] = {"connect server", "mark attendance", " "};

// WiFi settings
const char *ssid = "GalaxyA5150E4";
const char *password = "Password";
const char *websockets_server_host = "192.168.103.135";
const uint16_t websockets_server_port = 8080;

WebServer server(80);
WebSocketsClient webSocket;
bool webSocketConnected = false;
bool fingerprintSensorisWorking = false;

// Fingerprint sensor settings
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial2);

// Function prototypes
void taskUpdateBatteryCellData(void *parameter);
void taskDisplayUpdate(void *parameter);
void taskStartWebSocketClient(void *parameter);
void taskMarkAttendance(void *parameter);
void menuControlInterrupt();
void menuSelectInterrupt();
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
void sendFingerprintId(uint16_t id, const char *action);

// Task handles
TaskHandle_t taskHandleCellPercentage;
TaskHandle_t taskHandleStartWebSocketServer;
TaskHandle_t taskHandleMarkAttendance;

void setup()
{

  // Initialize I2C communication
  Wire.begin();

  // Initialize serial communication for fingerprint sensor
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

  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    // Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }
  finger.begin(57600);

  display.clearDisplay();

  WiFi.begin(ssid, password);

  // Create tasks
  xTaskCreate(taskUpdateBatteryCellData, "CellPercentage", 2048, NULL, 2, &taskHandleCellPercentage);
  xTaskCreate(taskDisplayUpdate, "DisplayUpdate", 2048, NULL, 2, NULL);
  // xTaskCreatePinnedToCore(taskEnableAPMode, "EnableAPMode", 4096, NULL, 2, &taskHandleEnableAPMode, CONFIG_ARDUINO_RUNNING_CORE);
  vTaskStartScheduler();
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

void taskDisplayUpdate(void *parameter)
{
  while (true)
  {
    display.clearDisplay();

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    if (WiFi.status() == WL_CONNECTED)
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

    display.display();
  }
}

void menuControlInterrupt()
{
  selectedMenuOption--;
  if (selectedMenuOption < 0)
  {
    selectedMenuOption = 1; // Wrap around to last option
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
    if (WiFi.status() != WL_CONNECTED)
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
      xTaskCreate(taskStartWebSocketClient, "WebSocketServer", 8192, NULL, 2, &taskHandleStartWebSocketServer);
    }
    break;

  case 1:
    // Mark attendance
    if (fingerprintSensorisWorking)
    {
      vTaskDelete(taskHandleMarkAttendance);
      menuItems[1] = "mark attendance";
      fingerprintSensorisWorking = false;
    }
    else
    {
      xTaskCreate(taskMarkAttendance, "MarkAttendance", 8192, NULL, 2, &taskHandleMarkAttendance);
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
      // convert id to char array and assign into menuItems[2]
      char buffer[10];
      sprintf(buffer, "%d", id);
      menuItems[2] = strdup(buffer);
      while (true)
      {
        fingerprintSensorisWorking = true;
        menuItems[1] = "stop enrollment";

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
                menuItems[1] = "mark attendance";
                menuItems[2] = " ";
                fingerprintSensorisWorking = false;
                break;
              }
              else
              {
                continue;
              }
            }
          }
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
      }
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
    menuItems[1] = "stop attendance";
    int p = finger.getImage();
    if (p == FINGERPRINT_OK)
    {
      p = finger.image2Tz();
      if (p == FINGERPRINT_OK)
      {
        p = finger.fingerFastSearch();
        if (p == FINGERPRINT_OK)
        {
          if (finger.confidence > 100)
          {
            sendFingerprintId((int)finger.fingerID, "attendance");
          }
          else
          {
            continue;
          }
        }
      }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void sendFingerprintId(int id, const char *action)
{
  DynamicJsonDocument doc(1024);
  doc[String(action)] = id;
  String jsonString;
  serializeJson(doc, jsonString);
  webSocket.sendTXT(jsonString);
}
