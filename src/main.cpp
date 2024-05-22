#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h>

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
const char *menuItems[] = {"connect server", "reset wifi"};

// WiFi settings
const char *ssid = "ESP32-AP";
const char *password = "password";
const char *configFile = "/config.json";

WebServer server(80);
bool APStarted = false;
bool wifiConnected = false;

// Function prototypes
void taskUpdateBatteryCellData(void *parameter);
void taskWiFiConnection(void *parameter);
void taskDisplayUpdate(void *parameter);
void taskEnableAPMode(void *parameter);
void menuControlInterrupt();
void menuSelectInterrupt();
bool connectToStoredWiFi();
void connectToWiFi(const char *ssid, const char *password);
void handleRoot();
void handleSave();
String getAvailableNetworks();
void resetWiFiConfig();

// Task handles
TaskHandle_t taskHandleCellPercentage;
TaskHandle_t taskHandleWiFiConnection;
TaskHandle_t taskHandleEnableAPMode;

void setup()
{
  Serial.begin(115200);

  // Initialize I2C communication
  Wire.begin();

  // configure the pin modes
  pinMode(menuControlBtn, INPUT_PULLUP);
  pinMode(menuItemSelectBtn, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(menuControlBtn), menuControlInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(menuItemSelectBtn), menuSelectInterrupt, FALLING);

  // Initialize Fuel Gauge
  // lipo.enableDebugging();
  if (lipo.begin() == false)
  {
    Serial.println("MAX17043 not detected. Freezing...");
    while (1)
      ;
  }

  lipo.quickStart();
  lipo.setThreshold(20);

  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("An error occurred while mounting SPIFFS...");
    return;
  }

  // Initialize OLED display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }

  display.display();
  delay(2000);
  display.clearDisplay();

  // Create tasks
  xTaskCreatePinnedToCore(taskUpdateBatteryCellData, "CellPercentage", 2048, NULL, 3, &taskHandleCellPercentage, CONFIG_ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(taskWiFiConnection, "WiFiConnection", 2048, NULL, 2, &taskHandleWiFiConnection, CONFIG_ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(taskDisplayUpdate, "DisplayUpdate", 2048, NULL, 1, NULL, CONFIG_ARDUINO_RUNNING_CORE);
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
      connectToStoredWiFi();
    }
    wifiConnected = true;
    vTaskDelay(1000 / portTICK_PERIOD_MS);
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
      for (int i = 0; i < sizeof(menuItems); i++)
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
    continue;
  }
}

void taskEnableAPMode(void *parameter)
{
  vTaskSuspend(taskHandleWiFiConnection);
  APStarted = true;
  wifiConnected = false;
  resetWiFiConfig();
  // Set ESP32 as an access point
  WiFi.softAP(ssid, password);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  Serial.println("Access Point started");
  while (true)
  {
    server.handleClient();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}
bool connectToStoredWiFi()
{
  // Open the file for reading
  File file = SPIFFS.open(configFile, FILE_READ);
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return false;
  }

  // Parse JSON from file
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  if (error)
  {
    Serial.println("Failed to read from file");
    return false;
  }

  const char *ssid = doc["ssid"];
  const char *password = doc["password"];

  file.close();

  Serial.println("Connecting to stored WiFi...");

  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi...");
    attempts++;
    if (attempts > 10)
    {
      Serial.println("Failed to connect to stored WiFi");
      return false;
    }
  }

  Serial.println("Connected to stored WiFi");
  Serial.println("IP Address: " + WiFi.localIP().toString());

  // Display message
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Connected to WiFi");
  display.setCursor(0, 10);
  display.println("IP Address:");
  display.println(WiFi.localIP().toString());
  display.display();

  return true;
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
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return;
  }

  // Serialize JSON to file
  if (serializeJson(doc, file) == 0)
  {
    Serial.println("Failed to write to file");
  }

  file.close();

  server.send(200, "text/html", "<html><body><h1 style='color:green;'>WiFi Credentials Saved Successfully!</h1></body></html>");

  delay(2000);
  server.close();
  // Connect to WiFi using the provided credentials
  connectToWiFi(ssid.c_str(), password.c_str());
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
void menuControlInterrupt()
{
  selectedMenuOption--;
  if (selectedMenuOption < 0)
  {
    selectedMenuOption = 2; // Wrap around to last option
  }
}
void menuSelectInterrupt()
{
  switch (selectedMenuOption)
  {
  case 0:
    // Connect to server
    break;
  case 1:
    // Enable the APMode task
    xTaskCreatePinnedToCore(taskEnableAPMode, "EnableAPMode", 4096, NULL, 2, &taskHandleEnableAPMode, CONFIG_ARDUINO_RUNNING_CORE);
    break;
  case 2:
    // Sleep device
    break;
  }
}
void resetWiFiConfig()
{
  // Delete the WiFi config file
  if (SPIFFS.remove(configFile))
  {
    Serial.println("WiFi config file deleted successfully");
  }
  else
  {
    Serial.println("Failed to delete WiFi config file");
  }
}
void connectToWiFi(const char *ssid, const char *password)
{

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi...");
    attempts++;
    if (attempts > 10)
    {
      Serial.println("Failed to connect to WiFi");
      return;
    }
  }

  APStarted = false;
  Serial.println("Connected to WiFi");
  Serial.println("IP Address: " + WiFi.localIP().toString());
  // resume the WiFi connection task and delete the APmode task
  vTaskDelete(taskHandleEnableAPMode);
  vTaskResume(taskHandleWiFiConnection);
}