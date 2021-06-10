#include "Adafruit_MAX31856.h"
#include <SoftwareSerial.h>
#include <string.h>


//pinout variables
int blueTx = 1;
int blueRx = 0;
int relayPins[2] = {22, 23}; //A9 A8
#define spi_SCK  13 //also wired to LED, but that is no problem. distributed to both max boards.
#define spi_SDO  12 //bus, labeled MISO on teensy
#define spi_SDI  11 //bus, labeled MOSI on teensy 
#define spi_CS0 9  // digital io (9)
#define spi_CS1 10  // reserved CS (10)
int spi_FAULT[2] = {5, 6}; //in case thermocouple amp is saturated. //UNUSED. faults are reported as SPI packets
bool heartbeat = HIGH;
bool heater_h = LOW;
float teensy_int_temp;
double setpoint = 0.0;

//setup bluetooth serial port
SoftwareSerial hc(blueRx,blueTx); //currently rfcomm1

extern float tempmonGetTemp(void);

//data formatting
//Formats as a json object, with different
//channels being different objects. so
//sends: '[{"topic": "Teensy1/A0", "data": temp}, {"topic":"Teensy1/A1", "data": temp}, ...]'
String buf;
String message;
String str1 = "S1=0";
String str2 = "S2=0";
String str3;
String teensyID = "Teensy3_SN"; //Sergei's Teensy ID 


//control variables
int relayStates[2] = {0, 0}; 
double temps[2] = {20, 20}; //temps in C
double cjTemps[2] = {20, 20}; //temperature of the max31856 cold junction, for monitoring failures.
bool tcFault[2] = {0, 0}; //fault flags.
double setpoints[2] = {10, 10};



//thermocouples. This option doesn't quite work for some reason, called "Software SPI"
//Adafruit_MAX31856 maxBoards[2] = {Adafruit_MAX31856(spi_CS0, spi_SDI, spi_SDO, spi_SCK), 
//                                  Adafruit_MAX31856(spi_CS1, spi_SDI, spi_SDO, spi_SCK)};

//Hardware SPI seems to work well though, so I guess the board is wired correctly.
Adafruit_MAX31856 maxBoards[2] = {Adafruit_MAX31856(spi_CS0), 
                                  Adafruit_MAX31856(spi_CS1)};


void setup(){

  //intialize relay pins
  for(int i = 0; i < 2; i++)
  {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW); //initial state is low
    pinMode(spi_FAULT[i], INPUT); //monitoring faults.
  }
  

  //Initialize Bluetooth Serial Port
  hc.begin(9600);
  Serial.begin(9600);
  Serial.println("Testing WiDaq board");
  Serial.println(teensyID);
  //clear the command buffer
  while(hc.available())
  {
    hc.read();
  }

  //initialize thermocouples
  for(int i = 0; i < 2; i++)
  {
    if(!maxBoards[i].begin())
    {
      Serial.print("Could not initialize max31856 object number "); Serial.println(i);
      continue;
    }
    maxBoards[i].setThermocoupleType(MAX31856_TCTYPE_T);
    maxBoards[i].setConversionMode(MAX31856_CONTINUOUS);
  }


 
}

void loop(){
  //function that looks at the input bluetooth module. 
  //will send to other functions if it finds anything. 
  //otherwise, returns here immediately.
  //checkForInputBlue(); 
  //checkForInputSer();
  measureTemperatures();
  //calculateRelayStates();
 
  teensy_int_temp = tempmonGetTemp();

   while(hc.available()) {
    str1 = hc.readString();
    Serial.println(str1);
    if(str1.substring(0,5) == "S1=0") {
      relayStates[0] = LOW;
      message = "Received: " + str1;
      Serial.println(message);
    } else if(str1.substring(0,5) == "S1=1") {
      relayStates[0] = HIGH;
      message = "Received: " + str1;
      Serial.println(message);
    }else if(str1.substring(0,5) == "S2=0") {
      relayStates[1] = LOW;
      message = "Received: " + str1;
      Serial.println(message);
    } else if(str1.substring(0,5) == "S2=1") {
      relayStates[1] = HIGH;
      message = "Received: " + str1;
      Serial.println(message);
    } 
    else if(str1.substring(0,2) == "T=") {
      str3 = str1.substring(2);
      setpoint = str3.toFloat();
      relayStates[0] = LOW;
      message = "Received set point: " + str3;
      Serial.println(message);
    }
    else {
      relayStates[0] = LOW;
      relayStates[1] = LOW;
      message = "Incorrect settings received:" + str1;
      Serial.println(message);
    }
  }

  Serial.print("TC temps: ");
  Serial.print(temps[0]); Serial.print(", "); Serial.print(temps[1]);
  Serial.print("   TC CJ temps: ");
  Serial.print(cjTemps[0]); Serial.print(", "); Serial.print(cjTemps[1]);
  Serial.print("   Relay States: ");
  Serial.print(relayStates[0]); Serial.print(", "); Serial.println(relayStates[1]);
  Serial.print("Teensy internal temperature:  "); Serial.println(teensy_int_temp);

  //hc.println("TC temps: ");
  //hc.print(temps[0]); Serial.print(", "); Serial.print(temps[1]);

  buf = String(temps[0]) + "&" + String(temps[1]) + "&" + String(heartbeat) 
  + "&" + String(teensy_int_temp) + "&" + String(setpoint) + "&" + String(relayStates[0]);
  hc.println(buf);
  Serial.println(buf);

  if(setpoint <= temps[0]) {
    relayStates[0] = LOW;
    heater_h = HIGH;
  } else if (setpoint >= temps[0] + 5.0) {
    relayStates[0] = HIGH;
    heater_h = LOW;
  } else if (setpoint < temps[0] + 5.0 && setpoint > temps[0] && heater_h) {
    relayStates[0] = LOW;
  }

  updateRelayStates();


  heartbeat = !heartbeat;
  
  delay(1000);


  
  //buf = formBluetoothPacket();
  //char bluetoothPacket[buf.length()];
  //buf.toCharArray(bluetoothPacket, buf.length());
  //hc.write(bluetoothPacket);
  
}

//measures the temperatures of the thermocouples
void measureTemperatures()
{
  
  for(int i = 0; i < 2; i++)
  {
    cjTemps[i] = maxBoards[i].readCJTemperature();
    temps[i] = maxBoards[i].readThermocoupleTemperature();
    //check for faults here
    uint8_t fault = maxBoards[i].readFault();
    if (fault) {
      if (fault & MAX31856_FAULT_CJRANGE) Serial.println("Cold Junction Range Fault");
      if (fault & MAX31856_FAULT_TCRANGE) Serial.println("Thermocouple Range Fault");
      if (fault & MAX31856_FAULT_CJHIGH)  Serial.println("Cold Junction High Fault");
      if (fault & MAX31856_FAULT_CJLOW)   Serial.println("Cold Junction Low Fault");
      if (fault & MAX31856_FAULT_TCHIGH)  Serial.println("Thermocouple High Fault");
      if (fault & MAX31856_FAULT_TCLOW)   Serial.println("Thermocouple Low Fault");
      if (fault & MAX31856_FAULT_OVUV)    Serial.println("Over/Under Voltage Fault");
      if (fault & MAX31856_FAULT_OPEN)    Serial.println("Thermocouple Open Fault");
    }
  }
}

//does the actual writing of relay states
void updateRelayStates()
{
  for(int i = 0; i < 2; i++)
  {
    digitalWrite(relayPins[i], relayStates[i]);
  }
}
