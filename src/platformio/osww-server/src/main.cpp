#include <Arduino.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <ESP32Time.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#ifdef OLED_ENABLED
	#include <SPI.h>
	#include <Wire.h>
	#include <Adafruit_GFX.h>
	#include <Adafruit_SSD1306.h>
#endif

#include "./utils/LedControl.h"
#include "./utils/MotorControl.h"

#include "FS.h"
#include "ESPAsyncWebServer.h"

/*
 * *************************************************************************************
 * ********************************* CONFIGURABLES *************************************
 * *************************************************************************************
 *
 * If you purchased the motor listed in the guide / Bill Of Materials, these default values are correct!
 *
 * durationInSecondsToCompleteOneRevolution = how long it takes the watch to complete one rotation on the winder.
 * directionalPinA = this is the pin that's wired to IN1 on your L298N circuit board
 * directionalPinB = this is the pin that's wired to IN2 on your L298N circuit board
 * ledPin = by default this is set to the ESP32's onboard LED. If you've wired an external LED, change this value to the GPIO pin the LED is wired to.
 * externalButton = OPTIONAL - If you want to use an external ON/OFF button, connect it to this pin 13. If you need to use another pin, change the value here.
 *
 * If you're using a NeoPixel equipped board, you'll need to change directionalPinA, directionalPinB and ledPin (pin 18 on most, I think) to appropriate GPIOs.
 * Failure to set these pins on NeoPixel boards will result in kernel panics.
 */
int durationInSecondsToCompleteOneRevolution = 8;
int directionalPinA = 25;
int directionalPinB = 26;
int ledPin = 0;
int externalButton = 13;

// OLED CONFIG
bool OLED_INVERT_SCREEN = false;
bool OLED_ROTATE_SCREEN_180 = false;
int SCREEN_WIDTH = 128; // OLED display width, in pixels
int SCREEN_HEIGHT = 64; // OLED display height, in pixels
int OLED_RESET = -1; // Reset pin number (or -1 if sharing Arduino reset pin)

// Home Assistant Configuration
const char* HOME_ASSISTANT_BROKER_IP = "192.168.1.251";
const char* HOME_ASSISTANT_USERNAME = "tulio";
const char* HOME_ASSISTANT_PASSWORD = "fyt202729";
/*
 * *************************************************************************************
 * ******************************* END CONFIGURABLES ***********************************
 * *************************************************************************************
 */

/*
 * DO NOT CHANGE THESE VARIABLES!
 */
String settingsFile = "/settings.json";
unsigned long rtc_offset;
unsigned long rtc_epoch;
unsigned long estimatedRoutineFinishEpoch;
unsigned long previousEpoch;
unsigned long startTimeEpoch;
bool reset = false;
bool routineRunning = false;
bool configPortalRunning = false;
bool screenSleep = false;
bool screenEquipped = OLED_ENABLED;
struct RUNTIME_VARS
{
	String status = "";
	String rotationsPerDay = "";
	String direction = "";
	String hour = "00";
	String minutes = "00";
	String winderEnabled = "1";
	String timerEnabled = "0";
};

/*
 * DO NOT CHANGE THESE VARIABLES!
 */
RUNTIME_VARS userDefinedSettings;
LedControl LED(ledPin);
WiFiManager wm;
AsyncWebServer server(80);
WiFiClient client;
ESP32Time rtc;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "192.168.1.246"); // Replace with your local NTP server IP
String winderooVersion = "3.0.0";

#if PWM_MOTOR_CONTROL
	MotorControl motor(directionalPinA, directionalPinB, true);
#else
	MotorControl motor(directionalPinA, directionalPinB);
#endif

#ifdef OLED_ENABLED
	Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#endif

#ifdef HOME_ASSISTANT_ENABLED
	#include <ArduinoHA.h>

	HADevice device;
	HAMqtt mqtt(client, device);

	// Define HA Sensors
	HASwitch ha_oledSwitch("oled");
	HANumber ha_rpd("rpd");
	HASelect ha_selectDirection("direction");
	HASwitch ha_timerSwitch("timerEnabled");
	HAButton ha_startButton("startButton");
	HAButton ha_stopButton("stopButton");
	HASelect ha_selectHours("hour");
	HASelect ha_selectMinutes("minutes");
	HASwitch ha_powerSwitch("power");
	HASensor ha_rssiReception("rssiReception");
	HASensor ha_activityState("activity");
#endif

void drawCentreStringToMemory(const char *buf, int x, int y)
{
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(buf, 0, y, &x1, &y1, &w, &h); //calc width of new string
    display.setCursor(x - (w / 2), y);
    display.print(buf);
}

static void drawStaticGUI(bool drawHeaderTitle = false, String title = "Winderoo") {
	if (OLED_ENABLED)
	{
		display.clearDisplay();

		display.setTextSize(1);
		display.setTextColor(WHITE);

		if (drawHeaderTitle)
		{
			drawCentreStringToMemory(title.c_str(), 64, 3);
		}
		// top horizontal line
		display.drawLine(0, 14, display.width(), 14, WHITE);
		// vertical line
		display.drawLine(64, 14, 64, 50, WHITE);
		// bottom horizontal line
		display.drawLine(0, 50, display.width(), 50, WHITE);

		display.setCursor(4, 18);
		display.println(F("TPD"));

		display.setCursor(71, 18);
		display.println(F("DIR"));

		display.display();
	}
}

static void drawTimerStatus() {
	if (OLED_ENABLED)
	{
		if (userDefinedSettings.timerEnabled == "1")
		{
			// right aligned timer
			display.fillRect(60, 51, 64, 13, BLACK);
			display.setCursor(60, 56);
			display.print("TIMER " + userDefinedSettings.hour + ":" + userDefinedSettings.minutes);
		}
		else
		{
			display.fillRect(60, 51, 68, 13, BLACK);
		}
	}
}

static void drawWifiStatus() {
	if (OLED_ENABLED)
	{
		// left aligned cell reception icon
		display.drawTriangle(4, 54, 10, 54, 7, 58, WHITE);
		display.drawLine(7, 58, 7, 62, WHITE);

		// Clear reception bars
		display.fillRect(12, 54, 58, 10, BLACK);

		if (WiFi.RSSI() > -50)
		{
			// Excellent reception - 4 bars
			display.fillRect(14, 55+8, 2, 2, WHITE);
			display.fillRect(18, 55+6, 2, 4, WHITE);
			display.fillRect(22, 55+4, 2, 6, WHITE);
			display.fillRect(26, 55+2, 2, 8, WHITE);
			if (HOME_ASSISTANT_ENABLED)	ha_rssiReception.setValue("Excellent");
		}
		else if (WiFi.RSSI() > -60)
		{
			// Good reception - 3 bars
			display.fillRect(14, 55+8, 2, 2, WHITE);
			display.fillRect(18, 55+6, 2, 4, WHITE);
			display.fillRect(22, 55+4, 2, 6, WHITE);
			if (HOME_ASSISTANT_ENABLED) ha_rssiReception.setValue("Good");
		}
		else if (WiFi.RSSI() > -70)
		{
			// Fair reception - 2 bars
			display.fillRect(14, 55+8, 2, 2, WHITE);
			display.fillRect(18, 55+6, 2, 4, WHITE);
			if (HOME_ASSISTANT_ENABLED) ha_rssiReception.setValue("Fair");
		}
		else
		{
			// Terrible reception - 1 bar
			display.fillRect(14, 55+8, 2, 2, WHITE);
			if (HOME_ASSISTANT_ENABLED) ha_rssiReception.setValue("Poor");
		}
	}
}

static void drawDynamicGUI() {
	if (OLED_ENABLED && !screenSleep)
	{

		display.fillRect(8, 25, 54, 25, BLACK);
		display.setCursor(8, 30);
		display.setTextSize(2);
		display.print(userDefinedSettings.rotationsPerDay);

		display.fillRect(66, 25, 62, 25, BLACK);
		display.setCursor(74, 30);
		display.print(userDefinedSettings.direction);
		display.setTextSize(1);

		drawWifiStatus();
		drawTimerStatus();

		display.display();
	}
}

static void drawNotification(String message) {
	if (OLED_ENABLED && !screenSleep)
	{
		display.setCursor(0, 0);
		display.drawRect(0, 0, 128, 14, WHITE);
		display.fillRect(0, 0, 128, 14, WHITE);
		display.setTextColor(BLACK);
		drawCentreStringToMemory(message.c_str(), 64, 3);
		display.display();
		display.setTextColor(WHITE);
		delay(200);
		display.setCursor(0, 0);
		display.drawRect(0, 0, 128, 14, BLACK);
		display.fillRect(0, 0, 128, 14, BLACK);
		display.setTextColor(WHITE);
		drawCentreStringToMemory(message.c_str(), 64, 3);

		// Underline notification, which is shared with Static GUI
		display.drawLine(0, 14, display.width(), 14, WHITE);
		display.display();
	}
}

template <int N> static void drawMultiLineText(const String (&message)[N]) {
	if (OLED_ENABLED && !screenSleep)
	{
		int yInitial = 20;
		int yOffset = 16;

		display.fillRect(0, 18, 128, 64, BLACK);

		for (int i = 0; i < N; i++)
		{
			if (i == 0)
			{
				drawCentreStringToMemory(message[i].c_str(), 64, yInitial);
			}
			else
			{
				drawCentreStringToMemory(message[i].c_str(), 64, yInitial + (yOffset * i));
			}
		}
	display.display();
	}
}

// Home Assistant Helper Functions
/**
 * @brief Returns the index corresponding to a given direction for Home Assistant.
 *
 * This function takes a direction string and returns an integer index that 
 * corresponds to the direction for Home Assistant. The mapping is as follows:
 * - "CCW" -> 0
 * - "BOTH" -> 1
 * - Any other string -> 2
 *
 * @param direction The direction string. Expected values are "CCW", "BOTH", or any other string.
 * @return int The index corresponding to the given direction.
 */
int getDirectionIndexForHomeAssistant(String direction)
{
	if (direction == "CCW")
	{
		return 0;
	}
	else if (direction == "BOTH")
	{
		return 1;
	}
	else
	{
		return 2;
	}
}

/**
 * @brief Converts a given minute value to an index used by Home Assistant.
 *
 * This function takes a minute value and returns a corresponding index
 * that is used by Home Assistant. The mapping is as follows:
 * - 0 minutes -> index 0
 * - 10 minutes -> index 1
 * - 20 minutes -> index 2
 * - 30 minutes -> index 3
 * - 40 minutes -> index 4
 * - 50 minutes -> index 5
 * 
 * If the minute value does not match any of the predefined cases, the function
 * returns 0 by default.
 *
 * @param minuteValue The minute value to be converted to an index.
 * @return The index corresponding to the given minute value.
 */
int getTimerMinutesIndexForHomeAssistant(int minuteValue)
{
	switch(minuteValue)
	{
		case 0:
			return 0;
		case 10:
			return 1;
		case 20:
			return 2;
		case 30:
			return 3;
		case 40:
			return 4;
		case 50:
			return 5;
		default:
			return 0;
	}
}

/**
 * Calculates the duration and estimated finish time of the winding routine
 *
 * @return epoch - estimated epoch when winding routine will finish
 */
unsigned long calculateWindingTime()
{
	int tpd = atoi(userDefinedSettings.rotationsPerDay.c_str());

	long totalSecondsSpentTurning = tpd * durationInSecondsToCompleteOneRevolution;

	// We want to rest every 3 minutes for 15 seconds
	long totalNumberOfRestingPeriods = totalSecondsSpentTurning / 180;
	long totalRestDuration = totalNumberOfRestingPeriods * 180;
	long finalRoutineDuration = totalRestDuration + totalSecondsSpentTurning;

	Serial.print("[STATUS] - Total winding duration: ");
	Serial.println(finalRoutineDuration);

	unsigned long epoch = rtc.getEpoch();
	unsigned long estimatedFinishTime = epoch + finalRoutineDuration;

	return estimatedFinishTime;
}

/**
 * Sets running conditions to TRUE & calculates winding time parameters
 */
void beginWindingRoutine()
{
	startTimeEpoch = rtc.getEpoch();
	previousEpoch = startTimeEpoch;
	routineRunning = true;
	userDefinedSettings.status = "Winding";
	Serial.println("[STATUS] - Begin winding routine");

	unsigned long finishTime = calculateWindingTime();
	estimatedRoutineFinishEpoch = finishTime;

	Serial.print("[STATUS] - Current time: ");
	Serial.println(rtc.getEpoch());

	Serial.print("[STATUS] - Estimated finish time: ");
	Serial.println(finishTime);

	drawNotification("Winding");
	if (HOME_ASSISTANT_ENABLED) ha_activityState.setValue("Winding");
}

/**
 * Calls external time API & updates ESP32's onboard real time clock
 */
void getTime() {
    timeClient.begin();
    timeClient.update();
    
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ptm = gmtime ((time_t *)&epochTime);
    
    int currentYear = ptm->tm_year + 1900;
    int currentMonth = ptm->tm_mon + 1;
    int currentDay = ptm->tm_mday;
    int currentHour = timeClient.getHours();
    int currentMinute = timeClient.getMinutes();
    int currentSecond = timeClient.getSeconds();
    
    Serial.printf("[STATUS] - Date: %d-%02d-%02d Time: %02d:%02d:%02d\n", 
        currentYear, currentMonth, currentDay,
        currentHour, currentMinute, currentSecond);
    
    rtc.setTime(currentSecond, currentMinute, currentHour, currentDay, currentMonth, currentYear);
    
    timeClient.end();
}

/**
 * Loads user defined settings from data file
 *
 * @param file_name fully qualified name of file to load
 * @return contents of file as a single string
 */
void loadConfigVarsFromFile(String file_name)
{
	String result = "";

	File this_file = LittleFS.open(file_name, "r");

	JsonDocument json;
	DeserializationError error = deserializeJson(json, this_file);

	if (!this_file || error)
	{
		Serial.println("[STATUS] - Failed to open configuration file, returning empty result");
	}
	while (this_file.available())
	{
		result += (char)this_file.read();
	}

	userDefinedSettings.status = json["savedStatus"].as<String>();						// Winding || Stopped = 7char
	userDefinedSettings.rotationsPerDay = json["savedTPD"].as<String>();				// min = 100 || max = 960
	userDefinedSettings.hour = json["savedHour"].as<String>();							// 00
	userDefinedSettings.minutes = json["savedMinutes"].as<String>();					// 00
	userDefinedSettings.timerEnabled = json["savedTimerState"].as<String>();			// 0 || 1
	userDefinedSettings.direction = json["savedDirection"].as<String>();				// CW || CCW || BOTH

	this_file.close();
}

/**
 * Saves user defined settings to data file
 *
 * @param file_name fully qualified name of file to save data to
 * @param contents entire contents to write to file
 * @return true if successfully wrote to file; else false
 */
bool writeConfigVarsToFile(String file_name, const RUNTIME_VARS& userDefinedSettings)
{
	File this_file = LittleFS.open(file_name, "w");

	JsonDocument json;

	if (!this_file)
	{
		Serial.println("[STATUS] - Failed to open configuration file");
		return false;
	}

	json["savedStatus"] = userDefinedSettings.status;
	json["savedTPD"] = userDefinedSettings.rotationsPerDay;
	json["savedHour"] = userDefinedSettings.hour;
	json["savedMinutes"] = userDefinedSettings.minutes;
	json["savedTimerState"] = userDefinedSettings.timerEnabled;
	json["savedDirection"] = userDefinedSettings.direction;

	if (serializeJson(json, this_file) == 0)
	{
		Serial.println("[STATUS] - Failed to write to configuration file");
		return false;
	}

	this_file.close();
	return true;
}

/**
 * 404 handler for webserver
 */
void notFound(AsyncWebServerRequest *request)
{
	// Handle HTTP_OPTIONS requests
	if (request->method() == 64)
	{
		AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", "Ok");
		request->send(response);
	}
	else
	{
		request->send(404, "text/plain", "Winderoo\n\n404 - Resource Not found");
	}
}

/**
 * API for front end
 */
void startWebserver()
{

	server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
	{
		AsyncResponseStream *response = request->beginResponseStream("application/json");
		JsonDocument json;
		json["status"] = userDefinedSettings.status;
		json["rotationsPerDay"] = userDefinedSettings.rotationsPerDay;
		json["direction"] = userDefinedSettings.direction;
		json["hour"] = userDefinedSettings.hour;
		json["minutes"] = userDefinedSettings.minutes;
		json["durationInSecondsToCompleteOneRevolution"] = durationInSecondsToCompleteOneRevolution;
		json["startTimeEpoch"] = startTimeEpoch;
		json["currentTimeEpoch"] = rtc.getEpoch();
		json["estimatedRoutineFinishEpoch"] = estimatedRoutineFinishEpoch;
		json["winderEnabled"] = userDefinedSettings.winderEnabled;
		json["timerEnabled"] = userDefinedSettings.timerEnabled;
		json["db"] = WiFi.RSSI();
		json["screenSleep"] = screenSleep;
		json["screenEquipped"] = screenEquipped;
		serializeJson(json, *response);

		request->send(response);

		// Update RTC time ref
		getTime();
	});

	server.on("/api/timer", HTTP_POST, [](AsyncWebServerRequest *request)
	{
		int params = request->params();

		for ( int i = 0; i < params; i++ )
		{
			AsyncWebParameter* p = request->getParam(i);

			if( strcmp(p->name().c_str(), "timerEnabled") == 0 )
			{
				userDefinedSettings.timerEnabled = p->value().c_str();
				if (HOME_ASSISTANT_ENABLED) ha_timerSwitch.setState(userDefinedSettings.timerEnabled.toInt());
			}
		}

		bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
		if ( !writeSuccess )
		{
			Serial.println("[ERROR] - Failed to write [timer] endpoint data to file");
			request->send(500, "text/plain", "Failed to write new configuration to file");
		}

		request->send(204);
	});

	server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
	{

		if (request->url() == "/api/power")
		{
			JsonDocument json;
			DeserializationError error = deserializeJson(json, data);

			if (error)
			{
				Serial.println("[ERROR] - Failed to deserialize [power] request body");
				request->send(500, "text/plain", "Failed to deserialize request body");
				return;
			}

			if (!json["winderEnabled"].is<bool>())
			{
				request->send(400, "text/plain", "Missing required field: 'winderEnabled'");
			}

			userDefinedSettings.winderEnabled = json["winderEnabled"].as<String>();

			if (userDefinedSettings.winderEnabled == "0")
			{
				Serial.println("[STATUS] - Switched off!");
				userDefinedSettings.status = "Stopped";
				routineRunning = false;
				motor.stop();
				display.clearDisplay();
				display.display();
				
				if (HOME_ASSISTANT_ENABLED) 
				{
					ha_powerSwitch.setState(false);
					ha_activityState.setValue("Stopped");
				}
			} else {
				drawStaticGUI(true);
				drawDynamicGUI();
				if (HOME_ASSISTANT_ENABLED) ha_powerSwitch.setState(true);
			}

			request->send(204);
		}

		if (request->url() == "/api/update")
		{
			JsonDocument json;
			DeserializationError error = deserializeJson(json, data);
			int arraySize = 7;
			String requiredKeys[arraySize] = {"rotationDirection", "tpd", "action", "hour", "minutes", "timerEnabled", "screenSleep"};

			if (error)
			{
				Serial.println("[ERROR] - Failed to deserialize [update] request body");
				request->send(500, "text/plain", "Failed to deserialize request body");
				return;
			}

			// validate request body
				for (int i = 0; i < arraySize; i++)
				{
					if(!json[requiredKeys[i]].is<JsonVariant>())
					{
						request->send(400, "text/plain", "Missing required field: '" + requiredKeys[i] +"'");
					}
				}

			// These values can be mutated / saved directly
			userDefinedSettings.hour = json["hour"].as<String>();
			userDefinedSettings.minutes = json["minutes"].as<String>();
			userDefinedSettings.timerEnabled = json["timerEnabled"].as<String>();

			// These values need to be compared to the current settings / running state
			String requestRotationDirection = json["rotationDirection"].as<String>();
			String requestTPD = json["tpd"].as<String>();
			String requestAction = json["action"].as<String>();
			screenSleep = json["screenSleep"].as<bool>();

			// Update Home Assistant state

			if (HOME_ASSISTANT_ENABLED)
			{
				ha_timerSwitch.setState(userDefinedSettings.timerEnabled.toInt());
				ha_selectHours.setState(userDefinedSettings.hour.toInt());
				ha_selectMinutes.setState(getTimerMinutesIndexForHomeAssistant(userDefinedSettings.minutes.toInt()));
				ha_oledSwitch.setState(!screenSleep); // Invert state because naming is hard...
				ha_rpd.setState(static_cast<int>(requestTPD.toInt()));
				ha_selectDirection.setState(getDirectionIndexForHomeAssistant(requestRotationDirection));
			}


			// Update motor direction
			if (strcmp(requestRotationDirection.c_str(), userDefinedSettings.direction.c_str()) != 0)
			{
				userDefinedSettings.direction = requestRotationDirection;
				motor.stop();
				delay(250);

				// Update motor direction
				if (userDefinedSettings.direction == "CW" )
				{
					motor.setMotorDirection(1);
				}
				else if (userDefinedSettings.direction == "CCW")
				{
					motor.setMotorDirection(0);
				}

				Serial.println("[STATUS] - direction set: " + userDefinedSettings.direction);
			}
			else
			{
				userDefinedSettings.direction = requestRotationDirection;
			}

			// Update (turns) rotations per day
			if (strcmp(requestTPD.c_str(), userDefinedSettings.rotationsPerDay .c_str()) != 0)
			{
				userDefinedSettings.rotationsPerDay = requestTPD;

				unsigned long finishTime = calculateWindingTime();
				estimatedRoutineFinishEpoch = finishTime;
			}

			// Update action (START/STOP)
			if ( strcmp(requestAction.c_str(), "START") == 0 )
			{
				if (!routineRunning)
				{
					userDefinedSettings.status = "Winding";
					beginWindingRoutine();
				}
			}
			else
			{
				motor.stop();
				routineRunning = false;
				userDefinedSettings.status = "Stopped";
				drawNotification("Stopped");
				if (HOME_ASSISTANT_ENABLED) ha_activityState.setValue("Stopped");
			}

			// Update screen sleep state
			if (screenSleep && OLED_ENABLED)
			{
				display.clearDisplay();
				display.display();
			}
			else
			{
				if (OLED_ENABLED)
				{
					// Draw gui with updated values from _this_ update request
					drawStaticGUI(true, userDefinedSettings.status);
					drawDynamicGUI();
				}
			}

			// Write new parameters to file
			bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
			if ( !writeSuccess )
			{
				Serial.println("[ERROR] - Failed to write [update] endpoint data to file");
				request->send(500, "text/plain", "Failed to write new configuration to file");
			}

			request->send(204);
		}
	});

	server.on("/api/reset", HTTP_GET, [](AsyncWebServerRequest *request)
	{
		Serial.println("[STATUS] - Received reset command");
		AsyncResponseStream *response = request->beginResponseStream("application/json");
		JsonDocument json;
		json["status"] = "Resetting";
		serializeJson(json, *response);
		request->send(response);

		reset = true;
	});

	server.serveStatic("/css/", LittleFS, "/css/").setCacheControl("max-age=31536000");
	server.serveStatic("/js/", LittleFS, "/js/").setCacheControl("max-age=31536000");
	server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

	server.onNotFound(notFound);

	DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
	DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
	DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Access-Control-Allow-Headers, Authorization, X-Requested-With");

	server.begin();
}

/**
 * Initialize File System
 */
void initFS()
{
	if (!LittleFS.begin(true))
	{
		Serial.println("[STATUS] - An error has occurred while mounting LittleFS");
	}
	Serial.println("[STATUS] - LittleFS mounted");
}

/**
 * Change LED's state
 *
 * @param blinkState 1 = slow blink, 2 = fast blink, 3 = snooze state
 */
void triggerLEDCondition(int blinkState)
{
	// remove any previous LED state (aka turn LED off)
	LED.off();
	delay(50);

	switch (blinkState)
	{
		case 1:
			LED.slowBlink();
			break;
		case 2:
			LED.fastBlink();
			break;
		case 3:
			LED.pwm();
			break;
		default:
			Serial.println("[WARN] - blinkState not recognized");
			break;
	}
}

/**
 * This is a non-block button listener function.
 * Credit to github OSWW contribution from user @danagarcia
 *
 * @param pauseInSeconds the amount of time to pause and listen
*/
void awaitWhileListening(int pauseInSeconds)
{
  // While waiting for the 1 second to pass, actively monitor/listen for button press.
  int delayEnd = millis() + (1000 * pauseInSeconds);
  while (millis() < delayEnd) {
    // get physical button state
    int buttonState = digitalRead(externalButton);

	if (buttonState == HIGH)
	{
		if (userDefinedSettings.winderEnabled == "0")
		{
			motor.stop();
			routineRunning = false;
			userDefinedSettings.status = "Stopped";
			Serial.println("[STATUS] - Switched off!");
			if (HOME_ASSISTANT_ENABLED) ha_activityState.setValue("Stopped");
		}
	}
	else
	{
		userDefinedSettings.winderEnabled == "1";
	}

  }
}

/**
 * Callback triggered from WifiManager when successfully connected to new WiFi network
 */
void saveParamsCallback()
{
	if (OLED_ENABLED)
	{
		display.clearDisplay();
		display.display();
		drawNotification("Connecting...");
	}
}

/**
 * Callback triggered from WifiManager when successfully connected to new WiFi network
 */
void saveWifiCallback()
{
	if (OLED_ENABLED)
	{
		display.clearDisplay();
		display.display();
		drawNotification("Connected to WiFi");
		String rebootingMessage[2] = {"Device is", "rebooting..."};
		drawMultiLineText(rebootingMessage);
	}

	// slow blink to confirm connection success
	triggerLEDCondition(1);

	ESP.restart();
	delay(1500);
}

// MQTT & Home Assistant Handlers
void mqttOnConnected()
{
	Serial.println("[STATUS] - MQTT connected!");
}

void mqttOnDisconnected()
{
	Serial.println("[STATUS] - MQTT disconnected!");
}

void onOledSwitchCommand(bool state, HASwitch* sender)
{
	if (state)
	{
		screenSleep = false;
		display.clearDisplay();
		drawStaticGUI(true);
		drawDynamicGUI();
	}
	else
	{
		screenSleep = true;
		display.clearDisplay();
		display.display();
	}

	sender->setState(state);
}

void onRpdChangeCommand(HANumeric number, HANumber* sender)
{
	char buffer[10];
	number.toStr(buffer);
	userDefinedSettings.rotationsPerDay = String(buffer);

	bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
	if ( !writeSuccess )
	{
		Serial.println("[ERROR] - Failed to write number state [MQTT]");
	}

	sender->setCurrentState(number);
}

void onSelectDirectionCommand(int8_t index, HASelect* sender) {
   switch (index) {
    case 0:
        // Option "CCW" was selected
		userDefinedSettings.direction = "CCW";
        break;

    case 1:
        // Option "BOTH" was selected
		userDefinedSettings.direction = "BOTH";
        break;

    case 2:
        // Option "CW" was selected
		userDefinedSettings.direction = "CW";
        break;

    default:
        // unknown option
        return;
    }

	bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
	if ( !writeSuccess )
	{
		Serial.println("[ERROR] - Failed to write direction select state [MQTT]");
	}

	sender->setState(index);
}

void onTimerSwitchCommand(bool state, HASwitch* sender)
{
	userDefinedSettings.timerEnabled = state ? "1" : "0";
	bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
	if ( !writeSuccess )
	{
		Serial.println("[ERROR] - Failed to write timer switch state [MQTT]");
	}

	sender->setState(state);
}

void handleHAStartButton(HAButton* sender)
{
	if (!routineRunning)
	{
		beginWindingRoutine();
	}
}

void handleHAStopButton(HAButton* sender)
{
	motor.stop();
	routineRunning = false;
	userDefinedSettings.status = "Stopped";
	drawNotification("Stopped");
	ha_activityState.setValue("Stopped");
}

void onSelectHoursCommand(int8_t index, HASelect* sender)
{
	// Ugly but more reliable
	switch (index) 
	{
		case 0:
			userDefinedSettings.hour = "00";
			break;
		case 1:
			userDefinedSettings.hour = "01";
			break;
		case 2:
			userDefinedSettings.hour = "02";
			break;
		case 3:
			userDefinedSettings.hour = "03";
			break;
		case 4:
			userDefinedSettings.hour = "04";
			break;
		case 5:
			userDefinedSettings.hour = "05";
			break;
		case 6:
			userDefinedSettings.hour = "06";
			break;
		case 7:
			userDefinedSettings.hour = "07";
			break;
		case 8:
			userDefinedSettings.hour = "08";
			break;
		case 9:
			userDefinedSettings.hour = "09";
			break;
		case 10:
			userDefinedSettings.hour = "10";
			break;
		case 11:
			userDefinedSettings.hour = "11";
			break;
		case 12:	
			userDefinedSettings.hour = "12";
			break;
		case 13:
			userDefinedSettings.hour = "13";
			break;
		case 14:
			userDefinedSettings.hour = "14";
			break;
		case 15:
			userDefinedSettings.hour = "15";
			break;
		case 16:
			userDefinedSettings.hour = "16";
			break;
		case 17:
			userDefinedSettings.hour = "17";
			break;
		case 18:
			userDefinedSettings.hour = "18";
			break;
		case 19:
			userDefinedSettings.hour = "19";
			break;
		case 20:
			userDefinedSettings.hour = "20";
			break;
		case 21:
			userDefinedSettings.hour = "21";
			break;
		case 22:
			userDefinedSettings.hour = "22";
			break;
		case 23:
			userDefinedSettings.hour = "23";
			break;
		default:
			return;
    }

	bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
	if ( !writeSuccess )
	{
		Serial.println("[ERROR] - Failed to write hours select state [MQTT]");
	}

	sender->setState(index);
}

void onSelectMinutesCommand(int8_t index, HASelect* sender)
{
	switch(index)
	{
		case 0:
			userDefinedSettings.minutes = "00";
			break;
		case 1:
			userDefinedSettings.minutes = "10";
			break;
		case 2:
			userDefinedSettings.minutes = "20";
			break;
		case 3:
			userDefinedSettings.minutes = "30";
			break;
		case 4:
			userDefinedSettings.minutes = "40";
			break;
		case 5:
			userDefinedSettings.minutes = "50";
			break;
		default:
			return;
	}

	bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
	if ( !writeSuccess )
	{
		Serial.println("[ERROR] - Failed to write minutes select state [MQTT]");
	}

	sender->setState(index);
}

void onPowerSwitchCommand(bool state, HASwitch* sender)
{
	userDefinedSettings.winderEnabled = state ? "1" : "0";

	if (userDefinedSettings.winderEnabled == "0")
	{
		Serial.println("[STATUS] - Switched off!");
		userDefinedSettings.status = "Stopped";
		routineRunning = false;
		motor.stop();
		display.clearDisplay();
		display.display();
		ha_activityState.setValue("Stopped");
	} else {
		drawStaticGUI(true);
		drawDynamicGUI();
	}

	bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
	if ( !writeSuccess )
	{
		Serial.println("[ERROR] - Failed to write power switch state [MQTT]");
	}

	sender->setState(state);
}

void setup()
{
	WiFi.mode(WIFI_STA);
	Serial.begin(115200);
	setCpuFrequencyMhz(160);

	// Timezone Brazil, Sao_Paulo
	timeClient.setTimeOffset(-10800);  // GMT-3 offset in seconds (-3 * 60 * 60)

	// Prepare pins
	pinMode(directionalPinA, OUTPUT);
	pinMode(directionalPinB, OUTPUT);
	pinMode(externalButton, INPUT);
	ledcSetup(LED.getChannel(), LED.getFrequency(), LED.getResolution());
	ledcAttachPin(LED_BUILTIN, LED.getChannel());

	// WiFi Manager config
	wm.setConfigPortalTimeout(3600);
	wm.setDarkMode(true);
	wm.setConfigPortalBlocking(false);
	wm.setHostname("Winderoo");
	wm.setSaveConfigCallback(saveWifiCallback);
	wm.setSaveParamsCallback(saveParamsCallback);

	userDefinedSettings.winderEnabled = true;

	if(OLED_ENABLED)
	{
		display.begin(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
		if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
		{
			Serial.println(F("SSD1306 allocation failed"));
			for(;;); // Don't proceed, loop forever
		}
		drawStaticGUI();

		int rotate = OLED_ROTATE_SCREEN_180 ? 2 : 4;
		display.clearDisplay();
		display.invertDisplay(OLED_INVERT_SCREEN);
		display.setRotation(rotate);
		drawNotification("Winderoo");
	}

	String savedNetworkMessage[2] = {"Connecting to", "saved network..."};
	drawMultiLineText(savedNetworkMessage);

	// Connect using saved credentials, if they exist
	// If connection fails, start setup Access Point
	if (wm.autoConnect("Winderoo Setup"))
	{
		initFS();
		Serial.println("[STATUS] - connected to saved network");

		// retrieve & read saved settings
		loadConfigVarsFromFile(settingsFile);
		
		if (!MDNS.begin("winderoo"))
		{
			Serial.println("[STATUS] - Failed to start mDNS");
			drawNotification("Failed to start mDNS");
		}
		MDNS.addService("_winderoo", "_tcp", 80);
		Serial.println("[STATUS] - mDNS started");

		// Configure Home Assistant
		if (HOME_ASSISTANT_ENABLED) 
		{
			byte mac[6];
			WiFi.macAddress(mac);
			device.setUniqueId(mac, sizeof(mac));

			device.setName("Winderoo");
			device.setManufacturer("mwood77");
			device.setModel("Winderoo");
			device.setSoftwareVersion(winderooVersion.c_str());
			device.enableSharedAvailability();

			ha_oledSwitch.setName("OLED");
			ha_oledSwitch.setIcon("mdi:overscan");
			ha_oledSwitch.setCurrentState(!screenSleep);
			ha_oledSwitch.onCommand(onOledSwitchCommand);

			ha_rpd.setName("Rotations Per Day");
			ha_rpd.setIcon("mdi:rotate-3d-variant");
			ha_rpd.setMin(100);
			ha_rpd.setMax(960);
			ha_rpd.setStep(10);
			ha_rpd.setCurrentState(static_cast<int32_t>(userDefinedSettings.rotationsPerDay.toInt()));
			ha_rpd.setOptimistic(true);
			ha_rpd.onCommand(onRpdChangeCommand);

			ha_selectDirection.setName("Direction");
			ha_selectDirection.setIcon("mdi:arrow-left-right");
			ha_selectDirection.setOptions("CCW;BOTH;CW");
			ha_selectDirection.onCommand(onSelectDirectionCommand);
			ha_selectDirection.setCurrentState(getDirectionIndexForHomeAssistant(userDefinedSettings.direction));

			ha_timerSwitch.setName("Timer Enabled");
			ha_timerSwitch.setIcon("mdi:timer");
			ha_timerSwitch.setCurrentState(userDefinedSettings.timerEnabled.toInt());
			ha_timerSwitch.onCommand(onTimerSwitchCommand);

			ha_startButton.setName("Start");
			ha_startButton.setIcon("mdi:play");
			ha_startButton.onCommand(handleHAStartButton);

			ha_stopButton.setName("Stop");
			ha_stopButton.setIcon("mdi:stop");
			ha_stopButton.onCommand(handleHAStopButton);

			ha_selectHours.setName("Hour");
			ha_selectHours.setIcon("mdi:timer-sand-full");
			ha_selectHours.setOptions("00;01;02;03;04;05;06;07;08;09;10;11;12;13;14;15;16;17;18;19;20;21;22;23");
			ha_selectHours.setCurrentState(userDefinedSettings.hour.toInt());
			ha_selectHours.onCommand(onSelectHoursCommand);

			ha_selectMinutes.setName("Minutes");
			ha_selectMinutes.setIcon("mdi:timer-sand-empty");
			ha_selectMinutes.setOptions("00;10;20;30;40;50");
			ha_selectMinutes.setCurrentState(userDefinedSettings.minutes.toInt());
			ha_selectMinutes.onCommand(onSelectMinutesCommand);

			ha_powerSwitch.setName("Power");
			ha_powerSwitch.setIcon("mdi:power");
			ha_powerSwitch.setCurrentState(userDefinedSettings.winderEnabled.toInt());
			ha_powerSwitch.onCommand(onPowerSwitchCommand);

			ha_activityState.setName("Status");
			ha_activityState.setIcon("mdi:information");
			ha_activityState.setValue(userDefinedSettings.status.c_str());

			ha_rssiReception.setName("WiFi Reception");
			ha_rssiReception.setIcon("mdi:antenna");

			mqtt.onConnected(mqttOnConnected);
			mqtt.onDisconnected(mqttOnDisconnected);
			mqtt.begin(HOME_ASSISTANT_BROKER_IP, HOME_ASSISTANT_USERNAME, HOME_ASSISTANT_PASSWORD);
			Serial.println("[STATUS] - HA Configured - Will attempt to connect to MQTT broker");

			if (OLED_ENABLED)
			{
				String configuredHomeAssistantMessage[2] = {"Configured for", "Home Assistant"};
				drawMultiLineText(configuredHomeAssistantMessage);
				delay(1500);
			}
		}

		if (OLED_ENABLED)
		{
			display.clearDisplay();
			drawStaticGUI();
			drawNotification("Connected to WiFi");
		}

		drawNotification("Getting time...");
		getTime();

		drawNotification("Starting webserver...");
		startWebserver();

		if (strcmp(userDefinedSettings.status.c_str(), "Winding") == 0)
		{
			beginWindingRoutine();
		}
		else
		{
			drawNotification("Winderoo");
		}
	}
	else
	{
		configPortalRunning = true;
		Serial.println("[STATUS] - WiFi Config Portal running");
		ledcWrite(LED.getChannel(), 255);

		String setupNetworkMessage[3] = {"Connect to", "\"Winderoo Setup\"", "wifi to begin"};
		drawMultiLineText(setupNetworkMessage);
	};
}

void loop()
{
	if (configPortalRunning)
	{
		wm.process();
		return;
	}

	if (reset)
	{
		if (OLED_ENABLED)
		{
			display.clearDisplay();
			drawNotification("Resetting");

			String rebootingMessage[2] = {"Device is", "rebooting..."};
			drawMultiLineText(rebootingMessage);
		}
		// fast blink
		triggerLEDCondition(2);

		Serial.println("[STATUS] - Stopping webserver");
		server.end();
		delay(600);
		Serial.println("[STATUS] - Stopping File System");
		LittleFS.end();
		delay(200);
		Serial.println("[STATUS] - Resetting Wifi Manager settings");
		wm.resetSettings();
		delay(200);
		Serial.println("[STATUS] - Restart device...");
		ESP.restart();
		delay(2000);
	}

	if (userDefinedSettings.timerEnabled == "1")
	{
		if (rtc.getHour(true) == userDefinedSettings.hour.toInt() &&
			rtc.getMinute() == userDefinedSettings.minutes.toInt() &&
			!routineRunning &&
			userDefinedSettings.winderEnabled == "1")
		{
			beginWindingRoutine();
			drawNotification("Winding Started");
		}
	}

	if (routineRunning)
	{
		unsigned long currentTime = rtc.getEpoch();

		if (rtc.getEpoch() < estimatedRoutineFinishEpoch)
		{

			// turn motor in direction
			motor.determineMotorDirectionAndBegin();
			int r = rand() % 100;

			if (r <= 25)
			{
				if ((strcmp(userDefinedSettings.direction.c_str(), "BOTH") == 0) && (currentTime - previousEpoch) > 180)
				{
					motor.stop();
					delay(3000);

					previousEpoch = currentTime;

					int currentDirection = motor.getMotorDirection();
					motor.setMotorDirection(!currentDirection);
					Serial.println("[STATUS] - Motor changing direction, mode: " + userDefinedSettings.direction);

					motor.determineMotorDirectionAndBegin();
				}

				if ((currentTime - previousEpoch) > 180)
				{
					Serial.println("[STATUS] - Pause");
					previousEpoch = currentTime;
					motor.stop();
					delay(3000);
				}
			}
		}
		else
		{
			// Routine has finished
			userDefinedSettings.status = "Stopped";
			routineRunning = false;
			motor.stop();
			if (OLED_ENABLED && !screenSleep)
			{
				drawNotification("Winding Complete");
				if (HOME_ASSISTANT_ENABLED) ha_activityState.setValue("Winding Complete");
			}

			bool writeSuccess = writeConfigVarsToFile(settingsFile, userDefinedSettings);
			if ( !writeSuccess )
			{
				Serial.println("[ERROR] - Failed to write updated configuration to file");
			}
		}
	}

	// non-blocking button listener
	awaitWhileListening(1);	// 1 second

	if (userDefinedSettings.winderEnabled == "0")
	{
		triggerLEDCondition(3);
	}
	else
	{
		drawDynamicGUI();
	}

	if (HOME_ASSISTANT_ENABLED)
	{
		mqtt.loop();
		// We report these every cycle as if the device's MQTT connection is dropped,
		// it will not be able to report its up-to-date state to Home Assistant.
		// This mitigates de-sync between HA and the web gui.
		ha_powerSwitch.setState(userDefinedSettings.winderEnabled.toInt());
		ha_activityState.setValue(userDefinedSettings.status.c_str());
	}

	wm.process();
}
