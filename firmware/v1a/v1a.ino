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

//setup bluetooth serial port
SoftwareSerial hc(blueRx,blueTx); //currently rfcomm2



//data formatting
//Formats as a json object, with different
//channels being different objects. so
//sends: '[{"topic": "Teensy1/A0", "data": temp}, {"topic":"Teensy1/A1", "data": temp}, ...]'
String buf;
String teensyID = "Teensy1"; //Evan's Teensy ID 


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
    maxBoards[i].setThermocoupleType(MAX31856_TCTYPE_K);
    maxBoards[i].setConversionMode(MAX31856_CONTINUOUS);
  }

  


 
}

void loop(){
  //function that looks at the input bluetooth module. 
  //will send to other functions if it finds anything. 
  //otherwise, returns here immediately.
  checkForInputBlue(); 
  //checkForInputSer();
  measureTemperatures();
  //calculateRelayStates();
  updateRelayStates();

  Serial.print("TC temps: ");
  Serial.print(temps[0]); Serial.print(", "); Serial.print(temps[1]);
  Serial.print("   TC CJ temps: ");
  Serial.print(cjTemps[0]); Serial.print(", "); Serial.print(cjTemps[1]);
  Serial.print("   Relay States: ");
  Serial.print(relayStates[0]); Serial.print(", "); Serial.println(relayStates[1]);

  
  delay(1000);
  

  
  //buf = formBluetoothPacket();
  //char bluetoothPacket[buf.length()];
  //buf.toCharArray(bluetoothPacket, buf.length());
  //hc.write(bluetoothPacket);
  
}

void checkForInputBlue()
{

  if (hc.available())
  {  
    String message = "";
    while(hc.available())
    {
      message += hc.readString();
    }
    parseMessage(message);
  }

  return;
}


void checkForInputSer()
{

  if (Serial.available())
  {  
    String message = "";
    while(Serial.available())
    {
      message += Serial.readString();
    }
    Serial.println("Got message: ");
    //temporary hand control of relays
    //expect input like 0,1 (off, on) 
    
    if(message.length() == 4)
    {
      char mes[4];
      message.toCharArray(mes, 4);
      Serial.println(mes);
      int statesTemp[2];
      statesTemp[0] = atoi(&mes[0]);
      statesTemp[1] = atoi(&mes[2]);
      for(int i = 0; i < 2; i++)
      {
        if(statesTemp[i] > 1)
        {
          statesTemp[i] = 1;
        }
        else if(statesTemp[i] < 0)
        {
          statesTemp[i] = 0;
        }
        relayStates[i] = statesTemp[i];
      }
    }
  }
  return;
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


//This function will blink and stop ADCs
//until told to restart via one of the bluetooth interfaces.
void blinkIdle()
{
  while(true)
  {
    //check here for input to restart. 
    if (hc.available())
    {  
      String message = "";
      String response;
      while(hc.available())
      {
        message += hc.readString();
      }
      response = parseMessage(message);
      //primary break statement for the loop. 
      if(response == "loop")
      {
        return;
      }
    }
    
    delay(500);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
  } 
}

//general parsing function that looks at 
//an input and tells the arduino what to do. 
//code in whatever features you want.
String parseMessage(String m)
{

  String retval;
  
  //re-enter the loop
  m.trim(); //trim trailing/leading spaces and such. 
  m.toLowerCase(); //case insensitive
  
  //soft reset, going back in the stack
  if(m == "start")
  {
    retval = "loop";
    return retval;
  }

  else if(m == "blink")
  {
    blinkIdle();
    return "";
  }

  else
  {
    return "";
  }
  
}
