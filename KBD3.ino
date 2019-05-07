/*
 * ESP8266 Web server with Web Socket to control a parabear dropper
 *
 */

#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WebSocketsServer.h>
#include <Hash.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <pins_arduino.h>
#include <Servo.h> 

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>

static const char ssid[] = "Aloft";
static const char password[] = "hiupthere";
MDNSResponder mdns;

static void writeLED(bool);

ESP8266WiFiMulti WiFiMulti;

ESP8266WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

Servo myservo;

Adafruit_BMP280 bme; // I2C - WMM changed address to 0x76 in ~/Arduino/libraries/Adafruit_BMP280_Library/Adafruit_BMP280.h

static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name = "viewport" content = "width = device-width, initial-scale = 1.0, maximum-scale = 1.0, user-scalable=0">
<title>Bear launcher</title>
<style>
"body { background-color: #808080; font-family: Arial, Helvetica, Sans-Serif; Color: #000000; }"
</style>
<script>
var websock;
function start() {
  websock = new WebSocket('ws://' + window.location.hostname + ':81/');
  websock.onopen = function(evt) { console.log('websock open'); };
  websock.onclose = function(evt) { console.log('websock close'); };
  websock.onerror = function(evt) { console.log(evt); };
  websock.onmessage = function(evt) {
    //console.log(evt);
    var aloftObj = JSON.parse(evt.data)

    if (aloftObj.command === 'ledon') {
      ledstatus.style.color = 'red';
    }
    if (aloftObj.command === 'ledoff') {
      ledstatus.style.color = 'black';
    }
    if (aloftObj.command === 'rearm') {
      rearm.style.color = 'black';
    }
    if (aloftObj.heartbeat !== "undefined") {
      var cnt = document.getElementById('status');
      cnt.innerHTML = aloftObj.heartbeat;
    }
    if (aloftObj.altitude !== "undefined") {
      var cnt = document.getElementById('altitude');
      altitude.innerHTML = aloftObj.altitude;
    }
    if (aloftObj.temperature !== "undefined") {
      var cnt = document.getElementById('temperature');
      temperature.innerHTML = aloftObj.temperature;
    }
    if (aloftObj.pressure !== "undefined") {
      var cnt = document.getElementById('barometer');
      barometer.innerHTML = aloftObj.pressure;
    }
  };
}
function buttonclick(e) {
  websock.send('{"command":"'+e.id+'"}');
}
</script>
</head>
<body onload="javascript:start();">
<h1>Internet of Kites</h1>
<table>
  <tr><td>Heartbeat</td><td id="status"> Connecting...</td></tr>
  <tr><td id="altitude">Not read yet</td><td> m</td></tr>
  <tr><td id="temperature">Not read yet</td><td> C</td></tr>
  <tr><td id="barometer">Not read yet</td><td> hPa</td></tr>
</table>
<p>
<div id="ledstatus"><b>LED</b></div>
<button id="ledon"  type="button" onclick="buttonclick(this);">On</button> 
<button id="ledoff" type="button" onclick="buttonclick(this);">Off</button>
<p><p>
<button id="rearm"  type="button" onclick="buttonclick(this);">Reload launcher</button> 
<p><p>
<button id="drop1" type="button" onclick="buttonclick(this);">Drop 1</button> 
<p><p>
<button id="drop2" type="button" onclick="buttonclick(this);">Drop 2</button> 
<p><p>
<button id="drop3" type="button" onclick="buttonclick(this);">Drop 3</button> 
<hr>
<form method='POST' action='/setAltimeter' enctype='multipart/form-data'>
  Sea level pressure  
  <input type="number" name="sealevelpressure"
           pattern="[0-9]+([\.][0-9]+)?" step="0.01"
            title="Sea level pressure" value = "%s"> KPa  
  Current elevation 
  <input type="number" name="elevation"
           pattern="[0-9]+([\.][0-9]+)?" step="0.1"
            title="Elevation" value="%s" min="0" max="5000"> meters
  <input type='submit' value='Set altimeter'>
   <p>
</form>
 <a href = "https://weather.gc.ca/city/pages/ns-40_metric_e.html">Conditions at Shearwater</a>
</body>
</html>
)rawliteral";

// Wemos D1 LED is 2, D1 is 5
const int LEDPIN = 2;
const int MOTORPIN = D8;
// Current LED status
bool LEDStatus;

#define POSN_LOADED 150
#define POSN_DROP1 120
#define POSN_DROP2 90
#define POSN_DROP3 30

int servoPosition = 0;
int targetPosition = POSN_DROP3;
float seaLevelPressure = 1035.0;
float groundPressure = 1030.0;
float groundElevation = 70.0;
float temperature = 15.0, pressure = 1000.0;
float currentAltitude = 0.0;
float barometricK = 0.0; // z = k T ln(Pe / Psea) <--- barometric equation

// Commands sent through Web Socket
const char LEDON[] = "{\"command\":\"ledon\"}";
const char LEDOFF[] = "{\"command\":\"ledoff\"}";
const char DROP1[] = "{\"command\":\"drop1\"}";
const char DROP2[] = "{\"command\":\"drop2\"}";
const char DROP3[] = "{\"command\":\"drop3\"}";
const char REARM[] = "{\"command\":\"rearm\"}";

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length)
{
  Serial.printf("webSocketEvent(%d, %d, ...)\r\n", num, type);
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\r\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\r\n", num, ip[0], ip[1], ip[2], ip[3], payload);
        // Send the current LED status
        if (LEDStatus) {
          webSocket.sendTXT(num, LEDON, strlen(LEDON));
        }
        else {
          webSocket.sendTXT(num, LEDOFF, strlen(LEDOFF));
        }
      }
      break;
    case WStype_TEXT:
      Serial.printf("[%u] get Text: %s\r\n", num, payload);

      if (strcmp(LEDON, (const char *)payload) == 0) {
        writeLED(true);
      }
      else if (strcmp(LEDOFF, (const char *)payload) == 0) {
        writeLED(false);
      }
      else if (strcmp(DROP1, (const char *)payload) == 0) {
        targetPosition = POSN_DROP1;
      }
      else if (strcmp(DROP2, (const char *)payload) == 0) {
        targetPosition = POSN_DROP2;
      }
      else if (strcmp(DROP3, (const char *)payload) == 0) {
        targetPosition = POSN_DROP3;
      }
      else if (strcmp(REARM, (const char *)payload) == 0) {
        targetPosition = POSN_LOADED;
      }
      else {
        Serial.println("Unknown command");
      }
      // send data to all connected clients
      webSocket.broadcastTXT(payload, length);
      break;
    case WStype_BIN:
      Serial.printf("[%u] get binary length: %u\r\n", num, length);
      hexdump(payload, length);

      // echo data back to browser
      webSocket.sendBIN(num, payload, length);
      break;
    default:
      Serial.printf("Invalid WStype [%d]\r\n", type);
      break;
  }
}

void handleRoot()
{
  size_t formFinal_len = strlen_P(INDEX_HTML) + 64; //room for form values
  char *formFinal = (char *)malloc(formFinal_len);
  if (formFinal == NULL) {
    Serial.println("formFinal malloc failed");
    return;
  }

  char seaLevelPressureStr[8]; 
  char elevationStr[8]; 
  dtostrf(seaLevelPressure, 3, 2, seaLevelPressureStr);
  dtostrf(groundElevation, 3, 2, elevationStr);
  
  snprintf_P(formFinal, formFinal_len, INDEX_HTML, seaLevelPressureStr, elevationStr);
  server.send(200, "text/html", formFinal);
  free(formFinal);
}

void handleSetAltimeter()
{
  String argi, argNamei;
  String formValue;

  for (uint8_t i=0; i<server.args(); i++) {
    Serial.print(server.argName(i));
    Serial.print('=');
    Serial.println(server.arg(i));
    if (server.argName(i).equals("elevation")) {
      groundElevation = server.arg(i).toFloat();
    }
    if (server.argName(i).equals("sealevelpressure")) {
      seaLevelPressure = server.arg(i).toFloat();
    }
  }
  // https://cdn-shop.adafruit.com/datasheets/BST-BMP180-DS000-09.pdf
  // altitude = 44330.0 * (1.0 - pow(groundPressure / seaLevelPressure, 0.190294957183635));
  // therefore at ground, we can extrapolate to seaLevelPressure
  //    seaLevelPressure = groundPressure / pow(1.0 - groundElevation/44330.0, 5.255)
  groundPressure = bme.readPressure()/100.0; // want hPa, library gives Pa
  seaLevelPressure = groundPressure / pow(1.0 - groundElevation/44330.0, 5.255);
  temperature = bme.readTemperature();
  Serial.print("P0: "); Serial.println(seaLevelPressure);
  Serial.print("Pe: "); Serial.println(groundPressure);
  Serial.print("T: "); Serial.println(temperature);
  
  // z = k T ln(Pe / Psea)
  // k = z / (T * ln(Pe / Psea)
  // T in absolute (Kelvin) scale
  barometricK =
    groundElevation / ( (temperature + 273.15) * log(groundPressure / seaLevelPressure));
  Serial.print("K: "); Serial.println(barometricK);

  handleRoot();
}

void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

static void writeLED(bool LEDon)
{
  LEDStatus = LEDon;
  // Note inverted logic for Adafruit HUZZAH board
  if (LEDon) {
    digitalWrite(LEDPIN, 0);
  }
  else {
    digitalWrite(LEDPIN, 1);
  }
}

void setup()
{
  pinMode(LEDPIN, OUTPUT);
  pinMode(MOTORPIN, OUTPUT);
  writeLED(false);

  Serial.begin(115200);

  myservo.attach(MOTORPIN);  // attaches the servo on GIO2 to the servo object 
  myservo.write(POSN_DROP3); delay(2000);
  // do stretching excercises and use up some time while TCP stack gets going
  #if 1
  myservo.write(0); delay(2000);
  myservo.write(45); delay(2000);
  myservo.write(0);
  #endif
  
//#define WAP_MODE

#ifdef WAP_MODE
  WiFi.disconnect(true);
  delay(2000);
  WiFi.mode(WIFI_AP);
  
  const char AP_Name[] = "Aloft";
  const char WiFiAPPSK[] = "canyouhearme";
    
  WiFi.softAP(AP_Name, WiFiAPPSK);
  Serial.print("WAP is ");Serial.println(AP_Name);
  
#else

  // static IP mode

  WiFi.mode(WIFI_STA);
  WiFi.hostname("Aloft");
  IPAddress ip(192, 168, 0, 70); // where xx is the desired IP Address
  IPAddress gateway(192, 168, 0, 1); // set gateway to match your network
  Serial.print("Setting static ip to : "); Serial.println(ip);
  IPAddress subnet(255, 255, 255, 0); 
  WiFi.config(ip, gateway, subnet);
  WiFi.begin("XXX_name_XXX", "XXX_password_XXX");
  
  Serial.println(WiFi.localIP());
#endif

  server.on("/", handleRoot);
  server.on("/setAltimeter", HTTP_POST, handleSetAltimeter);
  server.onNotFound(handleNotFound);

  server.begin();

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  delay(3000);

  if (!bme.begin()) {  
    Serial.println("Could not find a valid BMP280 sensor, check wiring!");
  }
}

char payload[256];
unsigned long healthCounter;
  
void loop()
{
  webSocket.loop();
  server.handleClient();
  healthCounter=millis();

  if(healthCounter%1000 ==0){
    char altitudeStr[8], temperatureStr[8], pressureStr[8];
    temperature = bme.readTemperature();
    pressure = bme.readPressure()/100.0;
    currentAltitude = bme.readAltitude(seaLevelPressure); 
    // z = k T ln(Pe / Psea)
    //currentAltitude = barometricK * (temperature + 273.15) * log( pressure / seaLevelPressure);
    currentAltitude -= groundElevation;
    dtostrf(currentAltitude, 3, 2, altitudeStr);
    dtostrf(temperature, 3, 1, temperatureStr); 
    dtostrf(pressure, 3, 2, pressureStr);
    
    os_sprintf(payload, 
    "{\"heartbeat\":%ld, \"position\":%d,\"target\":%d,\"altitude\":%s,\"temperature\":%s,\"pressure\":%s}", 
    healthCounter, servoPosition, targetPosition, altitudeStr, temperatureStr, pressureStr);

    Serial.println(payload);
    webSocket.broadcastTXT(payload, strlen(payload));
  }

  if (servoPosition < targetPosition){
    servoPosition++;
  }  
  else if (servoPosition > targetPosition){
    servoPosition--;
  }
  if (abs(servoPosition -targetPosition) > 2){
    myservo.write(servoPosition);               
    delay(10);
  }
}
