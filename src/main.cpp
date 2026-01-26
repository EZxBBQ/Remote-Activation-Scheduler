#include <Arduino.h>
#include "optional"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "LittleFS.h"       // for using littleFS file system on esp32
#include "WiFi.h"  
#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"
#include "HTTPClient.h"
#include "ArduinoJson.h"
#include "AsyncJson.h"
#include "time.h"           //for connecting to NTP server
#include "Wire.h"           // for I2C
#include "RTClib.h"         // for interacting with RS3231
static_assert(__cplusplus >= 201703L, "C++17 not enabled"); // confirm the use of c++17



// to do : fix the state test task to ensure state manager can respond to state test request to ask current state or change it (done)
// Global variables & definitions
// Tasks
xTaskHandle StateManagerTaskHandle = NULL;
xTaskHandle ErrorHandlingTaskHandle = NULL;

// State & event
typedef enum {
  STATE_IDLE,
  STATE_ACTIVE,
  STATE_CONFIG,
  STATE_ERROR
} state_enum;
typedef enum{
  EVENT_DEVICE_ON,
  EVENT_DEVICE_OFF,
  EVENT_DEVICE_FAIL,
  EVENT_WIFI_DISCONNECT,
  EVENT_NTP_FAIL,
  EVENT_RTC_FAIL,
  EVENT_ASK_STATE
} event_enum;

// data format
struct schedule_format{
  bool isOnce;
  bool dayMode; // 0 for week days, 1 for month days
  struct tm startTime;
  uint16_t pumpDurationSec;
};
typedef struct{
  event_enum eventCategory;
  std::optional<schedule_format> extra;
} request_format;

// queue
QueueHandle_t requestQueue;
QueueHandle_t responseQueue;

// WiFi & website related variables
// ensure WiFi use 2,4 GHz and use WPA2 for compatibility
const char* wifi_ssid = "punya orang";
const char* wifi_password = "b57aigqs";
AsyncWebServer webServer(80);
String receivedSchedule = "";
HTTPClient http;

// NTP related variables
const char* ntp_server = "pool.ntp.org";
const long gmt_offset = 25200; // GMT+7 (3600 x 7)
const int daylight_offset = 0;

// RS3231 related variables
RTC_DS3231 rtc;
DateTime currentDateTime;
const uint8_t sqw_pin = 7;
const uint8_t sda_pin = 8;
const uint8_t scl_pin = 9;
const uint8_t rtc_addr = 0x68;

// Device related variables
const uint8_t devicePin = 16;
bool isDeviceActive = false;



// WiFi related functions
void InitWifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.println(String(WiFi.status()));
    delay(1000);
  }
  Serial.println("Connected to : " + String(wifi_ssid));
  delay(1000);
}



// WebServer related functions
void StoreSchedule(AsyncWebServerRequest* request)
{
  request->send(200, "text/plain", "schedule is sent and stored!");

  /*
    JsonDocument is basic type without any allocation rule (newer library expect this)
    StaticJsonDocument is stored in stack (fixed size)
    DynamicJsonDocument is stored in heap (dynamic size)
    BasicJsonDocument is for custom allocation strategy
  */

  File scheduleFile = LittleFS.open("/schedule.txt", "w");
  if (!scheduleFile)
  {
    Serial.println("Schedule file missing or can't be opened");
    return;
  }
  scheduleFile.println(receivedSchedule);
  scheduleFile.close();

  // confirmation
  scheduleFile = LittleFS.open("/schedule.txt", "r");
  if (!scheduleFile)
  {
    Serial.println("Schedule file missing or can't be opened");
    return;
  }
  while (scheduleFile.available())
  {
    Serial.write(scheduleFile.read());
  }
  scheduleFile.close();
}

void ReceiveSchedule(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total)
{
  if (index == 0)
  {
    receivedSchedule = "";
    receivedSchedule.reserve(total);
  }

  for (size_t i = 0; i < len; i++)
  {
    receivedSchedule += (char)data[i];
  }
  Serial.println(receivedSchedule);
}

void InitWebServer()
{
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html");
  });
  webServer.on("/upload", HTTP_POST, StoreSchedule, NULL, ReceiveSchedule); // onRequest run after data fully transferred, onUpload and onBody run during transfer
  webServer.begin();
  Serial.println("IP Address : " + WiFi.localIP().toString());
  delay(1000);
}



// LittleFS related functions
void InitLittleFS()
{
  while (!LittleFS.begin(false))  // do not immedietly format upon mounting fail
  {
    Serial.println("LittleFS failed to initialized");
    delay(1000);
  } 
  Serial.println("LittleFS initialized");
  delay(1000);

  // sanity check
  File testFile = LittleFS.open("/test.txt", "r");
  if (!testFile)
  {
    Serial.println("Test file missing or can't be opened");
  }
  Serial.print("Test file : ");
  while (testFile.available())
  {
    Serial.write(testFile.read());  // get raw bytes
  }
  Serial.print("\n");
  testFile.close();
  delay(1000);
}



// NTP related functions
void InitNTP()
{
  configTime(gmt_offset, daylight_offset, ntp_server);
  Serial.println("NTP initializdd, using GMT+" + String(gmt_offset / 3600));
  delay(1000);

  // sanity check
  struct tm dateTime;
  getLocalTime(&dateTime);
  Serial.println(
    String("from NTP : ") + 
    dateTime.tm_year + String(":") + 
    dateTime.tm_mon + String(":") +
    dateTime.tm_mday + String(":") +
    dateTime.tm_wday + String(" ; ") +
    dateTime.tm_hour + String(":") + 
    dateTime.tm_min
  );
  delay(1000);
}



// RS3231 related functions
void InitRTC()
{
  while (!Wire.begin(sda_pin, scl_pin, 100000))
  {
    Serial.println("I2C failed to initialized");
    delay(1000);
  }
  Serial.println("I2C initialized");

  while (!rtc.begin())
  {
    Serial.println("RTC not found");
    delay(1000);
  }
  Serial.println("RTC initialized");
  rtc.disable32K();

  struct tm dateTime;
  getLocalTime(&dateTime);
  time_t dateTimeEpoch = mktime(&dateTime); // convert to absolute time in seconds
  dateTimeEpoch += gmt_offset;    // applied back offset
  rtc.adjust(DateTime(dateTimeEpoch));
  delay(1000);

  // sanity check
  currentDateTime = rtc.now();
  Serial.println(
    String("from RTC : ") + 
    currentDateTime.year() + String(":") + 
    currentDateTime.month() + String(":") +
    currentDateTime.day() + String(":") +
    currentDateTime.dayOfTheWeek() + String(" ; ") +
    currentDateTime.hour() + String(":") + 
    currentDateTime.minute()
  );
  delay(1000);
}



// Device related functions
void TurnOnDevice()
{
  isDeviceActive = true;
  digitalWrite(16, HIGH);
}

void TurnOffDevice()
{
  isDeviceActive = false;
  digitalWrite(16, LOW);
} 



// State manager
void StateManagerTask(void* params)
{
  state_enum currentState = STATE_IDLE;
  request_format request;

  while (true)
  {
    if (xQueueReceive(requestQueue, &request, portMAX_DELAY))
    {
      switch (request.eventCategory)
      {
        case EVENT_DEVICE_ON :
          // call function to turn on device
          TurnOnDevice();
          currentState = STATE_ACTIVE;
          break;

        case EVENT_DEVICE_OFF :
          // call function to turn off device
          TurnOffDevice();
          currentState = STATE_IDLE;
          break;

        case EVENT_DEVICE_FAIL :
          // call function to handle runtime device failure
          currentState = STATE_ERROR;
          break;

        case EVENT_WIFI_DISCONNECT :
          // call function to handle runtime wifi disconnect
          currentState = STATE_ERROR;
          break;

        case EVENT_NTP_FAIL :
          // call function to handle runtime NTP failure
          currentState = STATE_ERROR;
          break;

        case EVENT_RTC_FAIL :
          // call function to handle runtime RTC failure
          currentState = STATE_ERROR;
          break;

        case EVENT_ASK_STATE :
          xQueueSend(responseQueue, &currentState, pdMS_TO_TICKS(500));
          break;

        default :
          currentState = STATE_IDLE;
      }
    }
    vTaskDelay(pdTICKS_TO_MS(500));
  }
}



// Test Functions =====================================================
TaskHandle_t stateTestHandle = NULL;

void StateTest(void* params)
{
  bool isMenuShown = false;
  bool isEditMenuShown = false;
  String receivedInput = "";
  char inputEvent;
  request_format request;

  while (true)
  {
    if (!isMenuShown)
    {
      Serial.println("\n1 : current state");
      Serial.println("2 : turn on device");
      Serial.println("3 : turn off device");
      Serial.println("4 : trigger error");
      Serial.println("your input : ");
      isMenuShown = !isMenuShown;
    }
    
    while (Serial.available())
    {
      char inputChar = Serial.read();
      if (inputChar == '\n')
      {
        break;
      }
      receivedInput += inputChar;
    }

    if (receivedInput != NULL)
    {
      switch (receivedInput.charAt(0))
      {
        case '1' :
          state_enum receivedState;
          request.eventCategory = EVENT_ASK_STATE;
          if (xQueueSend(requestQueue, &request, pdMS_TO_TICKS(500)))
          {
            xQueueReceive(responseQueue, &receivedState, portMAX_DELAY);
            Serial.println("current state : " + String(receivedState));
          }
          break;

        case '2' :
          request.eventCategory = EVENT_DEVICE_ON;
          xQueueSend(requestQueue, &request, pdMS_TO_TICKS(500));
          Serial.println("State Changed");
          break;

        case '3' :
          request.eventCategory = EVENT_DEVICE_OFF;
          xQueueSend(requestQueue, &request, pdMS_TO_TICKS(500));
          Serial.println("State Changed");
          break;

        case '4' :
          request.eventCategory = EVENT_DEVICE_FAIL;
          xQueueSend(requestQueue, &request, pdMS_TO_TICKS(500));
          Serial.println("State Changed");
          break;

        default :
          Serial.println("invalid option!");
      }
      receivedInput = "";
    }
    
    vTaskDelay(pdTICKS_TO_MS(1000));
  }
}
// Test Functions End =====================================================



void setup() 
{
  Serial.begin(115200);
  pinMode(16, OUTPUT);
  InitWifi();
  InitLittleFS();
  InitNTP();
  InitRTC();
  InitWebServer();

  requestQueue = xQueueCreate(1, sizeof(request_format));
  if (requestQueue == NULL)
  {
    Serial.println("request queue failed!");
  }

  responseQueue = xQueueCreate(1, sizeof(state_enum));
  if (responseQueue == NULL)
  {
    Serial.println("response queue failed!");
  }

  xTaskCreatePinnedToCore(
    StateManagerTask,
    "State Manager Task",
    2048,
    NULL,
    1,
    &StateManagerTaskHandle,
    0
  );
  // xTaskCreatePinnedToCore(
  //   StateTest,
  //   "State Test",
  //   2048,
  //   NULL,
  //   1,
  //   &stateTestHandle,
  //   1
  // );
}

void loop() 
{ 
}

