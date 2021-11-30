/*
 * This code was from a project Arduino 1 for PGEA
 * This variant is NOT fully functional!
 * If there are unfinished functions, they will be commented
 * 
 * Arduino 1, PGEA
 * 
 * Authors: Daniel Gyudzhenev and Konstantin Kapanov
 */

#include <esp_littlefs.h>
#include <lfs.h>
#include <lfs_util.h>
#include <LITTLEFS.h>
#include <littlefs_api.h>

#include <sys/time.h>
#include <esp_sntp.h>

#include <Wire.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

RTC_DATA_ATTR WiFiClientSecure client;
RTC_DATA_ATTR IPAddress ip;
RTC_DATA_ATTR File sensorFile;

//if you need to read altitude,you need to know the sea level pressure
#define SEALEVELPRESSURE_HPA (1013.25)

//This Macro definition decide whether you use I2C or SPI
//When USEIIC is 1 means use I2C interface, When it is 0,use SPI interface
#define USEIIC 0

/*
  the default I2C address is 0x77, you can change it in Adafruit_BME280.h
*/

#if(USEIIC)
Adafruit_BME280 bme;
#else
#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23
#define SPI_CS 5
Adafruit_BME280 bme(SPI_CS, SPI_MOSI, SPI_MISO, SPI_SCK);
#endif

#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  30       /* Time ESP32 will go to sleep (in seconds) */
#define THREE_HOURS_IN_MILLIS 3 * 60 * 60 * 1000

#define SENSOR_FILE_LINES_TO_WRITE  10

// RTC Variable - Delete
RTC_DATA_ATTR bool toClear = true;

// Basic Auth:
// For write/edit operations use ADMIN_ID,
// For reading use USER_ID
String ADMIN_ID = "password is hidden :)";

// Placeholders:
// <value> - The decimal/number sent to the PI Point
// <time> - The time in Pi Timestamp format
String DEFAULT_POST_BODY = "{\"Good\": true,\"Questionable\": false,\"Value\": <value>}";
String DEFAULT_POST_BODY_PAST = "{\"Timestamp\": \"<time>\", \"Good\": true,\"Questionable\": false,\"Value\": <value>}";

// LittleFS
RTC_DATA_ATTR bool SENSOR_FILE_OPENED = false;
RTC_DATA_ATTR byte SENSOR_FILE_LINES = 0;

// DEBUG
bool DEBUG = true;

// WiFi
// Default values are for k230
char DEFAULT_SSID[] = "k230";
char DEFAULT_PASS[] = "password is hidden :)";

// Pi
// TODO: Able to automatically find point id's by name
char PI_SERVER_IP[] = "172.16.115.2";
int PI_SERVER_PORT = 443;

// Points
// These points are already configured in PI
String KK_TEMPERATURE_ID = "F1DPko_ui9mhw0GQccopxaoC9gIQAAAATEVOT1ZPU1I1MzBcS0stVEVNUA";
String KK_HUMIDITY_ID = "F1DPko_ui9mhw0GQccopxaoC9gIgAAAATEVOT1ZPU1I1MzBcS0stSFVNSURJVFk";
String KK_PRESSURE_ID = "F1DPko_ui9mhw0GQccopxaoC9gIwAAAATEVOT1ZPU1I1MzBcS0stUFJFU1NVUkU";

struct PiValue {
  String t_time;
  String webid;
  String value;
}

// NTP
RTC_DATA_ATTR bool NTP_SYNC;
RTC_DATA_ATTR byte SENSOR_MAX_LINES = 255;

void debug(String debug) {
  if (DEBUG) {
    Serial.print(debug);
  }
}

void initSensors() {
  debug("Initializing sensors...");

  if (!bme.begin()) {
    Serial.println("ERROR: Init Fail, Please Check your address or the wire you connected!!!");
    while (1);
  }
  debug("OK\n");
}

void testSerialRead() {
  if (Serial.available() > 0) {
    // read the incoming byte:
    int incomingByte = Serial.read();

    // say what you got:
    Serial.print("I received: ");
    Serial.println(incomingByte, DEC);
  }
}

// WiFi
bool checkWiFiConnection() {
  return WiFi.status() == WL_CONNECTED;
}

void connectWiFi(char ssid[], char pass[]) {
  debug("Connecting to " + String(ssid) + " with pass: " + String(pass) + "...");

  byte attempts = 0;

  // Disconnect to previous connection, if any
  WiFi.disconnect();

  delay(1000);

  WiFi.begin(ssid, pass);
  if (!(WiFi.status() == WL_CONNECTED)) {
    delay(300);
    debug(".");
    attempts++;

    if (attempts >= 10) {
      Serial.println("\nERROR: Can't connect to WiFi!");
      return;
    }
  }
  ip = WiFi.localIP();
  debug("OK\n");
}

// Pi
void connectPi(String ip, int port) {
  debug("Connecting to PI Server at: " + ip + " with port: " + String(port) + "...");

  if (!checkWiFiConnection()) {
    Serial.println("\nERROR: Not connected to WiFi!");
    return;
  }

  if (!client.connected()) {
    if (client.connect(PI_SERVER_IP, PI_SERVER_PORT))
      debug("OK\n");
    else {
      Serial.println("\nERROR: Can't connect to Pi Server!");
    }
  }
}

bool checkPiConnection() {
  return client.connected();
}

/**
   Send data to Pi - POST request

   TODO: Read POST response and determine if there is an error
*/
String sendToPi(String webid, String postBody, bool timeout) {
  String path = "/piwebapi/streams/" + webid + "/value";

  client.println("POST " + path + " HTTP/1.1");
  client.println("Authorization: " + ADMIN_ID);
  client.println("Host: " + String(PI_SERVER_IP));
  client.print("Content-Length: ");
  client.println(postBody.length());
  client.println();

  client.println(postBody);

  debug("Sent POST request! " + String(postBody));

  if (timeout) {
    unsigned long timeout = millis();
    while (!client.available()) {
      if (millis() - timeout > 5 * 1000) {
        Serial.println("ERROR: Request timeout!");
        return "Timeout";
      }
    }
  }

  // Read response
  String response;
  while (client.available()) {
    response += client.read();
  }

  return response;
}

// Send many values - NOT FINISHED
String sendToPi(PiValue* values, int arraySize, String postBody, bool timeout) {
  String fullBodyRequest = "["
  for(int i = 0; i < arraySize; i++) {
    PiValue val = values[i];
    String t_time = val.t_time;
    String webid = val.webid;
    String value = val.value;
    String body = val.body;

    String path = "/streamsets/{webId}/recorded";

    fullBodyRequest += 
  }
  

  client.println("POST " + path + " HTTP/1.1");
  client.println("Authorization: " + ADMIN_ID);
  client.println("Host: " + String(PI_SERVER_IP));
  client.print("Content-Length: ");
  client.println(postBody.length());
  client.println();

  client.println(postBody);

  debug("Sent POST request! " + String(postBody));

  if (timeout) {
    unsigned long timeout = millis();
    while (!client.available()) {
      if (millis() - timeout > 5 * 1000) {
        Serial.println("ERROR: Request timeout!");
        PI_REQUEST_STATUS = PI_REQUEST_TIMEOUT;
        return "Timeout";
      }
    }
  }

  // Read response
  String response;
  while (client.available()) {
    response += client.read();
  }

  PI_REQUEST_STATUS = PI_REQUEST_OK;
  return response;
}

// Flash

/*
   The while loop can be removed, if you don't care if LittleFS works
*/
void initLittleFS() {
  debug("Initializing LITTLEFS...");
  if (!LITTLEFS.begin(true)) {
    Serial.println("ERROR: Cannot mount LittleFS!");
    while (1);
  }

  debug("OK\n");

  if (!SENSOR_FILE_OPENED) {
    debug("Openning /sensor_values.txt...");
    sensorFile = LITTLEFS.open("/sensor_values.txt", "a+");
    if (!sensorFile) {
      Serial.println("ERROR: Failed to open sensor file!");
      while (1);
    }
    debug("OK\n");
  }

  SENSOR_FILE_OPENED = true;
}

void clearSensorFile() {
  if (SENSOR_FILE_OPENED) {
    closeSensorFile();
  }
  // Will clear the file
  sensorFile = LITTLEFS.open("/sensor_values.txt", "w");
  sensorFile.close();
  SENSOR_FILE_LINES = 0;
}

/*
   Closing a file means writing operation -> Takes time

   NOTE: Before doing OTA, close all files
*/
void closeSensorFile() {
  if (SENSOR_FILE_OPENED) {
    sensorFile.close();
    SENSOR_FILE_OPENED = false;
  }
}

// NTP

/**
   Sync RTC with NTP server
*/
void syncNTP() {
  // If already synced with NTP
  if (NTP_SYNC)
    return;
  if (checkWiFiConnection()) {
    debug("Syncing with NTP Server...");

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    byte attempts = 0;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
      attempts++;
      if (attempts >= 100) {
        Serial.println("ERROR: Cannot sync with NTP server!");
        NTP_SYNC = false;
        return;
      }

      delay(100);
    } // Wait for syncing to finish

    // Bulgaria timezone
    setenv("TZ", "EET-2", 1);
    tzset();

    NTP_SYNC = true;
    debug("OK\n");
  }
}

time_t getEpochTime() {
  if (!NTP_SYNC)
    return NULL;
  return time(NULL);
}

void writeSensorValues(String values[], int arraySize) {
  if (!NTP_SYNC) // Not NTP synced, can't save the values without the time
    return;
  if (SENSOR_FILE_LINES >= SENSOR_MAX_LINES) {
    Serial.println("ERROR: SENSOR_MAX_LINES limit is reached!");
    return;
  }
  if (!SENSOR_FILE_OPENED) {
    sensorFile = LITTLEFS.open("/sensor_values.txt", "a+");
    if (!sensorFile) {
      Serial.println("ERROR: Failed to open sensor file!");
      return;
    }
    SENSOR_FILE_OPENED = true;
  }

  sensorFile.seek(0, SeekEnd);

  debug("Writing to LITTLEFS...");

  // NOTE: 4 values per line as such TIME;VAL1;VAL2...VALn;
  sensorFile.print(String(getEpochTime()) + ";");
  for (int i = 0; i < arraySize; i++)
    sensorFile.print(values[i] + ";");
  sensorFile.println();

  SENSOR_FILE_LINES++;

  debug("OK\n");
}

void readSensorValues(String* lines, int arraySize) {
  if (!SENSOR_FILE_OPENED) {
    sensorFile = LITTLEFS.open("/sensor_values.txt", "a+");
    if (!sensorFile) {
      Serial.println("ERROR: Failed to open sensor file!");
      return;
    }
    SENSOR_FILE_OPENED = true;
  }

  sensorFile.seek(0, SeekSet);

  Serial.println("READING... " + String(sensorFile.position()));

  byte lineCount = 0;
  String line;
  while (sensorFile.available()) {
    char c = sensorFile.read();

    if (c == '\n') {
      // Serial.println(line);
      lines[lineCount++] = line;

      // TODO Finish the NTP
      line = "";
    } else {
      line += c;
    }
  }

  for (int i = 0; i < arraySize; i++) {
    String line1 = lines[i];
    if (line1 == "")
      continue;
    Serial.println(line1);
  }

  return;
}

void performDeepSleep() {
  Serial.flush();

  closeSensorFile();

  esp_deep_sleep_start();
}

// ------ SETUP ------
void setup() {
  Serial.begin(9600);

  // Init deep sleep
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);

  initSensors();

  initLittleFS();

  connectWiFi(DEFAULT_SSID, DEFAULT_PASS);

  connectPi(PI_SERVER_IP, PI_SERVER_PORT);

  if (toClear) {
    clearSensorFile();
    toClear = false;
  } // Delete

  String lines[SENSOR_FILE_LINES_TO_WRITE * 3];
  readSensorValues(lines, SENSOR_FILE_LINES_TO_WRITE * 3); // Delete

  syncNTP();
}

void loop() {
  // Checking for no connection
  if (!checkPiConnection()) {
    // Write to LITTLEFS
    writeSensorValues(new String[3] { String(bme.readTemperature()), String(bme.readHumidity()), String(bme.readPressure()) }, 3);

    Serial.println("ERROR: Not connected to Pi Server");

    client.stop();
    if (checkWiFiConnection()) {
      WiFi.disconnect();
      Serial.println("ERROR: Not connected to WiFi");
    }
  } else {
    // Send saved information
    if (SENSOR_FILE_LINES >= SENSOR_FILE_LINES_TO_WRITE) {
      String lines[SENSOR_FILE_LINES_TO_WRITE * 3];
      readSensorValues(lines, SENSOR_FILE_LINES_TO_WRITE * 3);
      for (int i = 0; i <= SENSOR_FILE_LINES_TO_WRITE * 3; i++) {
        String line = lines[i];
        String values[4];

        // Empty line
        if (line == "")
            break;

        // Max 256 values per line
        for (byte i = 0; i <= 255; i++) {
          String var = line.substring(0, line.indexOf(';'));
          line = line.substring(var.length() + 1, line.length());

          // End of line
          if (line == "")
            break;

          values[i] = var;
        }

        // TODO: Clean up the code
        // What to do if some values get sent, but others don't
        
        String t = var[0];
        String temp = var[1];
        String humidity = var[2];
        String pressure = var[3];

        String postTemp = DEFAULT_POST_BODY_PAST;
        postTemp.replace("<value>", String(bme.readTemperature()));
        postTemp.replace("<time>", t);
        sendToPi(KK_TEMPERATURE_ID, postTemp, true);

        String postHumidity = DEFAULT_POST_BODY_PAST;
        postHumidity.replace("<value>", humidity);
        postHumidity.replace("<time>", t);
        sendToPi(KK_HUMIDITY_ID, postHumidity, true);

        String postPressure = DEFAULT_POST_BODY_PAST;
        postPressure.replace("<value>", pressure);
        postPressure.replace("<time>", t);
        sendToPi(KK_PRESSURE_ID, postPressure, true);
      }

      clearSensorFile();
    }
    // Sensors need calibrate time !!!
    String postTemp = DEFAULT_POST_BODY;
    postTemp.replace("<value>", String(bme.readTemperature()));
    if(sendToPi(KK_TEMPERATURE_ID, postTemp, true) == "Timeout") {
      // Save to Flash
      
    }

    String postHumidity = DEFAULT_POST_BODY;
    postHumidity.replace("<value>", String(bme.readHumidity()));
    sendToPi(KK_HUMIDITY_ID, postHumidity, true);

    String postPressure = DEFAULT_POST_BODY;
    postPressure.replace("<value>", String(bme.readPressure()));
    sendToPi(KK_PRESSURE_ID, postPressure, true);


  }

  performDeepSleep();
}

//pin 36 i 38 da sa s i2c
