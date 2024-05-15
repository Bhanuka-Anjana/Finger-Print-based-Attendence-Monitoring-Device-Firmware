#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

volatile int selectedMenuOption = 0;
const int menuControlBtn = 12;
const int menuItemSelectBtn = 13;
// array for menu items
const char *menuItems[] = {"connect server", "reset wifi", "sleep device"};

// variable for switch context of the display
volatile int switchContext = 0;
// 0- menu
// 1- Access Point
// 2- Connecting to WiFi
// 3- Connected to WiFi
// 4- Reset WiFi
// 5- Sleep device
// 6- Connect to server

const char *ssid = "ESP32-AP";
const char *password = "password";
const char *configFile = "/config.json";
WebServer server(80);

// function prototypes
void handleRoot();
void handleSave();
String getAvailableNetworks();
boolean connectToStoredWiFi();
void connectToWiFi(const char *ssid, const char *password);
void listAllFiles();
void resetWiFiConfig();
void menuControlInterrupt();
void menuSelectInterrupt();
void initiateAccessPoint();

void setup()
{
  Serial.begin(115200);

  // Attach interrupts
  pinMode(menuControlBtn, INPUT_PULLUP);
  pinMode(menuItemSelectBtn, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(menuControlBtn), menuControlInterrupt, FALLING);
  attachInterrupt(digitalPinToInterrupt(menuItemSelectBtn), menuSelectInterrupt, FALLING);

  // initialize the display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }
  display.display();
  delay(2000);
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Initialize SPIFFS
  if (!SPIFFS.begin(true))
  {
    Serial.println("An error occurred while mounting SPIFFS...");
    return;
  }

  // Check if WiFi config file exists
  if (SPIFFS.exists(configFile))
  {
    Serial.println("Found WiFi config file, attempting to connect...");
    if (connectToStoredWiFi())
    {
      return;
    }
  }

  // Set ESP32 as an access point
  initiateAccessPoint();
}
void loop()
{
  server.handleClient();
  switch (switchContext)
  {
  case 0:
    display.clearDisplay();
    display.setCursor(0, 0);
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
    break;
  case 1:
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Access Point");
    display.display();
    break;
  case 2:
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Connecting to WiFi...");
    display.display();
    break;
  case 3:
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Connected to WiFi");
    display.display();
    // switch context to menu
    switchContext = 0;
    break;

  case 4:
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Reset WiFi");
    display.display();
    break;
  case 5:
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Sleep device");
    display.display();
    break;
  case 6:
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Connect to server");
    display.display();
    break;
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
void connectToWiFi(const char *ssid, const char *password)
{
  // change context to to connecting to wifi
  switchContext = 2;
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

  // change context to connected to wifi
  switchContext = 3;
  Serial.println("Connected to WiFi");
  Serial.println("IP Address: " + WiFi.localIP().toString());
}
boolean connectToStoredWiFi()
{
  // change context to to connecting to wifi
  switchContext = 2;
  // Open the file for reading
  File file = SPIFFS.open(configFile, FILE_READ);
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return false;
  }

  // Parse JSON from file
  StaticJsonDocument<200> doc;
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

  // change context to connected to wifi
  switchContext = 3;
  return true;
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

  // Restart the ESP32 as an access point
  initiateAccessPoint();
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
    Serial.println("Connect to server");
    // change display context
    switchContext = 6;
    break;
  case 1:
    // Reset WiFi
    resetWiFiConfig();
    break;
  case 2:
    // Sleep device
    Serial.println("Sleep device");
    // change display context
    switchContext = 5;
    break;
  }
}
void initiateAccessPoint()
{
  // Set ESP32 as an access point
  WiFi.softAP(ssid, password);

  // Start server
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Access Point started");
  // change display context
  switchContext = 1;
}