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
const char *menuItems[] = {"connect server", "mark attendance", ""};

// WiFi settings
const char *ssid = "GalaxyA5150E4";
const char *password = "Password";
const char *websockets_server_host = "192.168.203.135";
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
void enrollFingerPrint(void *parameter);
void menuControlInterrupt();
void menuSelectInterrupt();
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
void sendFingerprintId(int id, const char *action);

// Task handles
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
  pinMode(23, OUTPUT);

  attachInterrupt(digitalPinToInterrupt(menuControlBtn), menuControlInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(menuItemSelectBtn), menuSelectInterrupt, FALLING);

  WiFi.begin(ssid, password);

  // Create tasks
  xTaskCreate(taskUpdateBatteryCellData, "CellPercentage", 2048, NULL, 2, NULL);
  xTaskCreate(taskDisplayUpdate, "DisplayUpdate", 2048, NULL, 2, NULL);
}

void loop()
{
  // Not used in FreeRTOS
}

void taskUpdateBatteryCellData(void *parameter)
{
  // Initialize Fuel Gauge
  // lipo.enableDebugging();
  if (lipo.begin() == false)
  {
    // Serial.println("MAX17043 not detected. Freezing...");
    while (1)
      ;
  }
  lipo.setThreshold(20);

  while (true)
  {
    // Read battery percentage from MAX17043
    lipo.quickStart();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    cellPercentage = lipo.getSOC();
    lowBattery = lipo.getAlert();
    lipo.sleep();
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

void taskDisplayUpdate(void *parameter)
{
  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    // Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }

  display.clearDisplay();
  while (true)
  {
    display.clearDisplay();

    display.setTextSize(1);
    display.setTextColor(WHITE);
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

    display.setCursor(0, 8);
    for (int i = 0; i < 3; i++)
    {
      if (selectedMenuOption == i)
      {
        display.print(">");
      }
      else
      {
        display.print("");
      }
      if (i == 2)
      {
        // inverse the color of the last item
        display.setTextColor(BLACK, WHITE);
      }
      display.println(menuItems[i]);
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
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

      // Turn off the finger print sensor
      digitalWrite(23, LOW);

      // delete task itself
      vTaskDelete(NULL);
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

      // Turn off the finger print sensor
      digitalWrite(23, LOW);
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
      menuItems[2] = "";
      fingerprintSensorisWorking = false;

      // Turn off the finger print sensor
      digitalWrite(23, LOW);
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
      fingerprintSensorisWorking = true;
      xTaskCreate(enrollFingerPrint, "EnrollFingerPrint", 8192, (void *)id, 2, NULL);
    }
    break;
  }

  default:
    break;
  }
}

void taskMarkAttendance(void *parameter)
{
  if (!webSocketConnected)
  {
    menuItems[2] = "Connect to server";
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    menuItems[2] = "";
    vTaskDelete(NULL);
  }

  // Turn on the finger print sensor
  digitalWrite(23, HIGH);
  finger.begin(57600);
  vTaskDelay(100 / portTICK_PERIOD_MS);

  while (true)
  {
    fingerprintSensorisWorking = true;
    menuItems[1] = "stop attendance";
    menuItems[2] = "Place finger";
    int p = finger.getImage();
    if (p == FINGERPRINT_OK)
    {
      p = finger.image2Tz();

      if (p == FINGERPRINT_OK)
      {
        menuItems[2] = "Image taken";
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        p = finger.fingerFastSearch();
        if (p == FINGERPRINT_OK)
        {
          if (finger.confidence > 70)
          {
            menuItems[2] = "Marked!";
            sendFingerprintId((int)finger.fingerID, "attendance");
          }
          else
          {
            menuItems[2] = "No match";
          }
          vTaskDelay(2000 / portTICK_PERIOD_MS);
          continue;
        }
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void sendFingerprintId(int id, const char *action)
{
  DynamicJsonDocument doc(1024);
  doc["action"] = action;
  doc["id"] = id;
  String jsonString;
  serializeJson(doc, jsonString);
  webSocket.sendTXT(jsonString);
}

void enrollFingerPrint(void *parameter)
{
  int id = (int)parameter;

  // Turn on the finger print sensor
  digitalWrite(23, HIGH); 
  finger.begin(57600);
  vTaskDelay(100 / portTICK_PERIOD_MS);

  while (true)
  {
    int p = -1;
    while (p != FINGERPRINT_OK)
    {
      p = finger.getImage();
      switch (p)
      {
      case FINGERPRINT_OK:
        menuItems[2] = "Image taken";
        break;
      case FINGERPRINT_NOFINGER:
        menuItems[2] = "No finger";
        break;
      case FINGERPRINT_PACKETRECIEVEERR:
        menuItems[2] = "Communication error";
        break;
      case FINGERPRINT_IMAGEFAIL:
        menuItems[2] = "Imaging error";
        break;
      default:
        menuItems[2] = "Unknown error";
        break;
      }
    }

    p = finger.image2Tz(1);
    switch (p)
    {
    case FINGERPRINT_OK:
      menuItems[2] = "Image converted";
      break;
    case FINGERPRINT_IMAGEMESS:
      menuItems[2] = "Image too messy";
      continue;
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      menuItems[2] = "Communication error";
      continue;
      break;
    case FINGERPRINT_FEATUREFAIL:
      menuItems[2] = "Could not find fingerprint features";
      continue;
      break;
    case FINGERPRINT_INVALIDIMAGE:
      menuItems[2] = "Could not find fingerprint features";
      continue;
      break;
    default:
      menuItems[2] = "Unknown error";
      continue;
      break;
    }

    menuItems[2] = "Remove finger";
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    p = 0;
    while (p != FINGERPRINT_NOFINGER)
    {
      p = finger.getImage();
    }
    p = -1;
    menuItems[2] = "Place again";
    while (p != FINGERPRINT_OK)
    {
      p = finger.getImage();
      switch (p)
      {
      case FINGERPRINT_OK:
        menuItems[2] = "Image taken";
        break;
      case FINGERPRINT_NOFINGER:
        menuItems[2] = "No finger";
        break;
      case FINGERPRINT_PACKETRECIEVEERR:
        menuItems[2] = "Communication error";
        break;
      case FINGERPRINT_IMAGEFAIL:
        menuItems[2] = "Imaging error";
        break;
      default:
        menuItems[2] = "Unknown error";
        break;
      }
    }

    // OK success!

    p = finger.image2Tz(2);
    switch (p)
    {
    case FINGERPRINT_OK:
      menuItems[2] = "Image converted";
      break;
    case FINGERPRINT_IMAGEMESS:
      menuItems[2] = "Image too messy";
      continue;
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      menuItems[2] = "Communication error";
      continue;
      break;
    case FINGERPRINT_FEATUREFAIL:
      menuItems[2] = "Could not find fingerprint features";

      continue;
      break;
    case FINGERPRINT_INVALIDIMAGE:
      menuItems[2] = "Could not find fingerprint features";
      continue;
      break;
    default:
      menuItems[2] = "Unknown error";
      continue;
      break;
    }

    // OK converted!

    p = finger.createModel();
    if (p == FINGERPRINT_OK)
    {
      menuItems[2] = "Prints matched!";
    }
    else
    {
      menuItems[2] = "Prints did not match!";
      continue;
    }

    p = finger.storeModel(id);
    if (p == FINGERPRINT_OK)
    {
      // Send the fingerprint id to the server
      sendFingerprintId(id, "enroll_confirm");

      // Turn off the finger print sensor
      digitalWrite(23, LOW);

      menuItems[2] = "Stored!";
      fingerprintSensorisWorking = false;

      vTaskDelay(3000 / portTICK_PERIOD_MS);

      menuItems[2] = "";

      // delete task itself
      vTaskDelete(NULL);
      break;
    }
    else
    {
      menuItems[2] = "Failed to store";
      continue;
    }
  }
}