#include <Arduino.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ESP32Time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

#include "./utils/LedControl.h"
#include "./utils/MotorControl.h"

#include "FS.h"
#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include "ESPAsyncWebServer.h"

/*
 * *************************************************************************************
 * ********************************* CONFIGURABLES *************************************
 * *************************************************************************************
 *
 * durationInSecondsToCompleteOneRevolution = how long it takes the watch to complete one rotation on the winder.
 * 												If you purchased the motor listed in the guide / Bill Of Materials, then this default value is correct!
 * directionalPinA = this is the pin that's wired to IN1 on your L298N circuit board
 * directionalPinB = this is the pin that's wired to IN2 on your L298N circuit board
 * ledPin = by default this is set to the ESP32's onboard LED. If you've wired an external LED, change this value to the GPIO pin the LED is wired to.
 * externalButton = OPTIONAL - If you want to use an external ON/OFF button, connect it to this pin 13. If you need to use another pin, change the value here.
 */
int durationInSecondsToCompleteOneRevolution = 8;
int directionalPinA = 25;
int directionalPinB = 26;
int ledPin = 0;
int externalButton = 13;
/*
 * *************************************************************************************
 * ******************************* END CONFIGURABLES ***********************************
 * *************************************************************************************
 */

/*
 * DO NOT CHANGE THESE VARIABLES!
 */
String timeURL = "http://worldtimeapi.org/api/ip";
String settingsFile = "/settings.txt";
const char *service_name = "Winderoo Setup";
const char *pop = "winderoo";
unsigned long rtc_offset;
unsigned long rtc_epoch;
unsigned long estimatedRoutineFinishEpoch;
unsigned long previousEpoch;
unsigned long startTimeEpoch;
bool reset = false;
bool routineRunning = false;
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
MotorControl motor(directionalPinA, directionalPinB);
WiFiManager wm;
AsyncWebServer server(80);
HTTPClient http;
WiFiClient client;
ESP32Time rtc;

static Device Winderoo("Winderoo", "custom.device.winder");

/**
 * Calclates the duration and estimated finish time of the winding routine
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



	unsigned long epoch = rtc.getEpoch();
	unsigned long estimatedFinishTime = epoch + finalRoutineDuration;

	return estimatedFinishTime;
}

/**
 * Calls external time API & updates ESP32's onboard real time clock
 */
void getTime()
{
	http.begin(client, timeURL);
	int httpCode = http.GET();

	if (httpCode > 0)
	{
		DynamicJsonDocument doc(2048);
		deserializeJson(doc, http.getStream());
		const unsigned long epoch = doc["unixtime"];
		const unsigned long offset = doc["raw_offset"];

		rtc.offset = offset;
		rtc.setTime(epoch);
	}

	http.end();
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


	unsigned long finishTime = calculateWindingTime();
	estimatedRoutineFinishEpoch = finishTime;
}

/**
 * Rainmaker request handler. This is a similar behaviou to the POST /update endpoint.
 */
void write_callback(Device *device, Param *param, const param_val_t val, void *priv_data, write_ctx_t *ctx) {
	const char *device_name = device->getDeviceName();
	const char *param_name = param->getParamName();

	if(strcmp(param_name, "tpd") == 0) {
		int speed = val.val.i;
		
		std::string ss = std::to_string(speed);
        userDefinedSettings.rotationsPerDay = ss.c_str();
        param->updateAndReport(val);
	}

	if(strcmp(param_name, "rotationDirection") == 0) {
		const char* dMode = val.val.s;

		userDefinedSettings.direction = dMode;

		motor.stop();
		delay(250);

		// Update motor direction
		if (userDefinedSettings.direction == "CW" ) {
			motor.setMotorDirection(1);
		} else if (userDefinedSettings.direction == "CCW") {
			motor.setMotorDirection(0);
		}

        param->updateAndReport(val);
	}

	if (strcmp(param_name, "Control") == 0) {
        bool controlStartStop = val.val.b;
        
		if (controlStartStop) {
            if (!routineRunning) {
              userDefinedSettings.status = "Winding";
              beginWindingRoutine();
            }
          } else {
            motor.stop();
            routineRunning = false;
            userDefinedSettings.status = "Stopped";
          }

        param->updateAndReport(val);
	}
}

/**
 * Loads user defined settings from data file
 *
 * @param file_name fully qualified name of file to load
 * @return contents of file as a single string
 */
String loadConfigVarsFromFile(String file_name)
{
	String result = "";

	File this_file = LittleFS.open(file_name, "r");

	if (!this_file)
	{
		return result;
	}
	while (this_file.available())
	{
		result += (char)this_file.read();
	}

	this_file.close();
	return result;
}

/**
 * Saves user defined settings to data file
 *
 * @param file_name fully qualified name of file to save data to
 * @param contents entire contents to write to file
 * @return true if successfully wrote to file; else false
 */
bool writeConfigVarsToFile(String file_name, String contents)
{
	File this_file = LittleFS.open(file_name, "w");

	if (!this_file)
	{
		return false;
	}

	int bytesWritten = this_file.print(contents);

	if (bytesWritten == 0)
	{
		return false;
	}

	this_file.close();
	return true;
}

/**
 * Parses substrings from user settings file & maps to runtime variables
 *
 * @param settings non-delimited string of settings
 */
void parseSettings(String settings)
{
	String savedStatus = settings.substring(0, 7);		 // Winding || Stopped = 7char
	String savedTPD = settings.substring(8, 11);		 // min = 100 || max = 960
	String savedHour = settings.substring(12, 14);		 // 00
	String savedMinutes = settings.substring(15, 17);	 // 00
	String savedTimerState = settings.substring(18, 19); // 0 || 1
	String savedDirection = settings.substring(20);		 // CW || CCW || BOTH

	userDefinedSettings.status = savedStatus;
	userDefinedSettings.rotationsPerDay = savedTPD;
	userDefinedSettings.hour = savedHour;
	userDefinedSettings.minutes = savedMinutes;
	userDefinedSettings.timerEnabled = savedTimerState;
	userDefinedSettings.direction = savedDirection;
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
 * API for front end interaction
 */
void startWebserver()
{

	server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
			  {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument json(1024);
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
    serializeJson(json, *response);

    request->send(response);

    // Update RTC time ref
    getTime(); });

	server.on("/api/power", HTTP_POST, [](AsyncWebServerRequest *request)
			  {
    int params = request->params();
    
    for ( int i = 0; i < params; i++ ) {
      AsyncWebParameter* p = request->getParam(i);

        if( strcmp(p->name().c_str(), "winderEnabled") == 0 ) {
          userDefinedSettings.winderEnabled = p->value().c_str();

          if (userDefinedSettings.winderEnabled == "0") {
            userDefinedSettings.status = "Stopped";
            routineRunning = false;
            motor.stop();
          }
        }
    }
    
    request->send(204); });

	server.on("/api/update", HTTP_POST, [](AsyncWebServerRequest *request)
			  {
    int params = request->params();
    
    for ( int i = 0; i < params; i++ ) {
      AsyncWebParameter* p = request->getParam(i);
    
        if( strcmp(p->name().c_str(), "rotationDirection") == 0 ) {
          userDefinedSettings.direction = p->value().c_str();

          motor.stop();
          delay(250);

          // Update motor direction
          if (userDefinedSettings.direction == "CW" ) {
            motor.setMotorDirection(1);
          } else if (userDefinedSettings.direction == "CCW") {
            motor.setMotorDirection(0);
          }
        }
    
        if( strcmp(p->name().c_str(), "tpd") == 0 ) {
          const char* newTpd = p->value().c_str();

          if (strcmp(newTpd, userDefinedSettings.rotationsPerDay.c_str()) != 0) {
            userDefinedSettings.rotationsPerDay = p->value().c_str();

            unsigned long finishTime = calculateWindingTime();
            estimatedRoutineFinishEpoch = finishTime;
          }
        }

        if( strcmp(p->name().c_str(), "hour") == 0 ) {
          userDefinedSettings.hour = p->value().c_str();
        }
		
		if( strcmp(p->name().c_str(), "timerEnabled") == 0 ) {
          userDefinedSettings.timerEnabled = p->value().c_str();
		}

        if( strcmp(p->name().c_str(), "minutes") == 0 ) {
          userDefinedSettings.minutes = p->value().c_str();
        }

        if( strcmp(p->name().c_str(), "action") == 0) {
          if ( strcmp(p->value().c_str(), "START") == 0 ) {
            if (!routineRunning) {
              userDefinedSettings.status = "Winding";
              beginWindingRoutine();
            }
          } else {
            motor.stop();
            routineRunning = false;
            userDefinedSettings.status = "Stopped";
          }
        }
    }

    String configs = userDefinedSettings.status + "," + userDefinedSettings.rotationsPerDay + "," + userDefinedSettings.hour + "," + userDefinedSettings.minutes + "," +  userDefinedSettings.timerEnabled + "," + userDefinedSettings.direction;

    bool writeSuccess = writeConfigVarsToFile(settingsFile, configs);

    if ( !writeSuccess ) {
      request->send(500);
    }

    request->send(204); });

	server.on("/api/reset", HTTP_GET, [](AsyncWebServerRequest *request)
			  {
		AsyncResponseStream *response = request->beginResponseStream("application/json");
		DynamicJsonDocument json(1024);
		json["status"] = "Resetting";
		serializeJson(json, *response);
		request->send(response);
		
		reset = true; });

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
		break;
	}
}


void sysProvEvent(arduino_event_t *sys_event)
{
  switch (sys_event->event_id) {
    case ARDUINO_EVENT_PROV_START:
// #if CONFIG_IDF_TARGET_ESP32
//       Serial.printf("\nProvisioning Started with name \"%s\" and PoP \"%s\" on BLE\n", service_name, pop);
//       printQR(service_name, pop, "ble");
// #else
      Serial.printf("\nProvisioning Started with name \"%s\" and PoP \"%s\" on SoftAP\n", service_name, pop);
      printQR(service_name, pop, "softap");
// #endif
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("\nConnected to Wi-Fi!\n");
      break;
  }
}

/**
 * Handles Rainmaker initialization and service availability for UI
 */
// void initializeRainmaker() {
// 	// Expose functionality & define interface types for Rainmaker UI
// 	Param rpdParam("Rotations Per Day", ESP_RMAKER_PARAM_RANGE, value((int)userDefinedSettings.rotationsPerDay.toInt()), PROP_FLAG_READ | PROP_FLAG_WRITE);
// 	rpdParam.addUIType(ESP_RMAKER_UI_SLIDER);
// 	// rpdParam.addBounds(value(100), value(userDefinedSettings.rotationsPerDay.toInt()), value(960));
// 	rpdParam.addBounds(value(100), value(480), value(960));
// 	Winderoo.addParam(rpdParam);

// 	const char* directionalModes[] = { "CW", "BOTH", "CCW" };
// 	Param modeDirection("Direction", "custom.param.direction", value((char*)userDefinedSettings.direction.c_str()), PROP_FLAG_READ | PROP_FLAG_WRITE);
// 	modeDirection.addValidStrList(directionalModes, 3);
// 	modeDirection.addUIType(ESP_RMAKER_UI_DROPDOWN);
// 	Winderoo.addParam(modeDirection);

// 	Param toggleStartStop("Control", "custom.param.toggle", value((char*)userDefinedSettings.status.c_str()), PROP_FLAG_READ | PROP_FLAG_WRITE);
// 	toggleStartStop.addUIType(ESP_RMAKER_UI_TOGGLE);
// 	// Winderoo.assignPrimaryParam("Control");

// 	Winderoo.addCb(write_callback);
// 	winder_node.addDevice(Winderoo);

// 	RMaker.enableSchedule();
//     RMaker.enableScenes();
//     RMaker.start();

//     WiFi.onEvent(sysProvEvent);
	
// 	#if CONFIG_IDF_TARGET_ESP32
// 		WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM, WIFI_PROV_SECURITY_1, pop, service_name);
// 	#else
// 		WiFiProv.beginProvision(WIFI_PROV_SCHEME_SOFTAP, WIFI_PROV_SCHEME_HANDLER_NONE, WIFI_PROV_SECURITY_1, pop, service_name);
// 	#endif
// }

/**
 * Callback triggered from WifiManager when successfully connected to new WiFi network
 */
void saveWifiCallback()
{
	// slow blink to confirm connection success
	triggerLEDCondition(1);
	ESP.restart();
	delay(2000);
}

void setup()
{
	// WiFi.mode(WIFI_STA);
	Serial.begin(115200);
	// setCpuFrequencyMhz(80);

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

	userDefinedSettings.winderEnabled = true;

	// Connect using saved credentials, if they exist
	// If connection fails, start setup Access Point
	// if (wm.autoConnect("Winderoo Setup"))
	// {
		initFS();

		// retrieve & read saved settings
		String savedSettings = loadConfigVarsFromFile(settingsFile);
		parseSettings(savedSettings);

		MDNS.begin("winderoo");
		// MDNS.addService("_winderoo", "_tcp", 80);

		if (strcmp(userDefinedSettings.status.c_str(), "Winding") == 0)
		{
			beginWindingRoutine();
		}

		Node winder_node = RMaker.initNode("Winderoo Device");
		// Expose functionality & define interface types for Rainmaker UI
		Param rpdParam("Rotations Per Day", ESP_RMAKER_PARAM_RANGE, value((int)userDefinedSettings.rotationsPerDay.toInt()), PROP_FLAG_READ | PROP_FLAG_WRITE);
		rpdParam.addUIType(ESP_RMAKER_UI_SLIDER);
		rpdParam.addBounds(value(100), value((int)userDefinedSettings.rotationsPerDay.toInt()), value(960));
		Winderoo.addParam(rpdParam);

		const char* directionalModes[] = { "CW", "BOTH", "CCW" };
		Param modeDirection("Direction", "custom.param.direction", value((char*)userDefinedSettings.direction.c_str()), PROP_FLAG_READ | PROP_FLAG_WRITE);
		modeDirection.addValidStrList(directionalModes, 3);
		modeDirection.addUIType(ESP_RMAKER_UI_DROPDOWN);
		Winderoo.addParam(modeDirection);

		Param toggleStartStop("Control", "custom.param.toggle", value((char*)userDefinedSettings.status.c_str()), PROP_FLAG_READ | PROP_FLAG_WRITE);
		toggleStartStop.addUIType(ESP_RMAKER_UI_TOGGLE);
		// Winderoo.assignPrimaryParam("Control");

		Winderoo.addCb(write_callback);
		winder_node.addDevice(Winderoo);

		RMaker.enableSchedule();
		RMaker.enableScenes();
		RMaker.start();

		WiFi.onEvent(sysProvEvent);

	
// #if CONFIG_IDF_TARGET_ESP32
// 	WiFiProv.beginProvision(WIFI_PROV_SCHEME_BLE, WIFI_PROV_SCHEME_HANDLER_FREE_BTDM, WIFI_PROV_SECURITY_1, pop, service_name);
// #else
	WiFiProv.beginProvision(WIFI_PROV_SCHEME_SOFTAP, WIFI_PROV_SCHEME_HANDLER_NONE, WIFI_PROV_SECURITY_1, pop, service_name);
// #endif
		// getTime();
		startWebserver();

	// }
	// else
	// {
	// 	ledcWrite(LED.getChannel(), 255);
	// };
}

void loop()
{

	if (reset)
	{
		// fast blink
		triggerLEDCondition(2);

		server.end();
		delay(600);
		LittleFS.end();
		delay(200);
		wm.resetSettings();
		delay(200);
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
					motor.determineMotorDirectionAndBegin();
				}

				if ((currentTime - previousEpoch) > 180)
				{
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
		}
	}

	// get physical button state
	int buttonState = digitalRead(externalButton);

	if (buttonState == HIGH)
	{
		if (userDefinedSettings.winderEnabled == "0")
		{
			userDefinedSettings.status = "Stopped";
			routineRunning = false;
			motor.stop();
		}
	}

	if (userDefinedSettings.winderEnabled == "0")
	{
		triggerLEDCondition(3);
	}

	wm.process();
	delay(1000);
}
