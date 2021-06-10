#include "Adafruit_MAX31856.h"
#include <string.h>


//pinout variables
int blueTx = 1;
int blueRx = 0;
int relayPins[2] = {22, 23}; //A9 A8
#define LED 13
#define spi_SCK  13 //also wired to LED, but that is no problem. distributed to both max boards.
#define spi_SDO  12 //bus, labeled MISO on teensy
#define spi_SDI  11 //bus, labeled MOSI on teensy 
#define spi_CS0 9  // digital io (9)
#define spi_CS1 10  // reserved CS (10)
int spi_FAULT[2] = {5, 6}; //in case thermocouple amp is saturated. //UNUSED. faults are reported as SPI packets

//setup bluetooth serial port on pins 1 and 0, Serial1 in Teensy's hardware libraries
#define hc Serial1

//the serial data from bluetooth is kept organized and protected by start and end characters
const char startMarker = '<';
const char endMarker = '>';

//data formatting
//Formats as a json object, with different
//channels being different objects. so
//sends: '[{"topic": "Teensy1/A0", "data": temp}, {"topic":"Teensy1/A1", "data": temp}, ...]'
String buf;
String teensyID = "Teensy1"; //Evan's Teensy ID 

String state; //the state of the teensy, the set of actions it takes at each loop cycle

int communicationErrors = 0; //keeping track of any errors that are caught by checksum

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
  delay(1000);
  Serial.begin(9600);
  delay(1000);
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

  state = "default"; //enter in the default state

}


//this loop is set up to be the "top" of each iteration. at each iteration,
//the "state machine" that it is supposed to run is executed in a separate function.
//the state will change based on user input, which is organized by a case-switch and
//a memory of the present state.
void loop(){

  String newstate = checkForInputBlue(); //loads the state string using inputs from the bluetooth
  //String newstate = checkForInputSer(); //loads the state string using input from the USB-serial input
  
  //check if the message from bluetooth contains the flag for changing states
  if(newstate.indexOf("statechange") != -1)
  {
    Serial.print("State is presently: ");
    Serial.println(state);
    state = newstate;
    Serial.print("Changing to: ");
    Serial.println(state);
  }

  //this large if statement controls which function is going to be performed during this loop
  //default state
  if(state == "statechange: default")
  {
    stateDualTempControl();
  }
  else if(state == "statechange: blink")
  {
    //toggle the LED pin using the opposite of its present value
    digitalWrite(LED, !digitalRead(LED));
  }
  //a specific mode for stress testing comms efficiency
  else if(state == "statechange: checksum")
  {
    stateChecksumMode();
  }
  else if(state == "statechange: blueconfig")
  {
    stateBlueConfig();
  }
  else
  {
    stateDualTempControl(); //the default again
  }
  Serial.print("So far there have been "); 
  Serial.print(communicationErrors);
  Serial.println(" communication errors");
  delay(1000);
}

//a more strongly protected bluetooth receiver than
//the serial receiver. had issues, so implemented start/end markers.
//it waits for the end marker to appear, if it received a start marker. 

//it seems like the HC06 has a maximum receive buffer of 62 chars or so...
//***Must do for large RX sizes, find the location of your hardware files for
//Teensy. On mac, one "Shows contents" of the Teensyduino application, then
//Java/hardware/teensy/avr/cores/teensy4/HardwareSerial1.cpp and change the RX_BUFFER_SIZE
//to 4096 (or however large you need)
String checkForInputBlue()
{
  String message = "";
  boolean rxInProgress = false; //if we are receiving, i.e. inside < ... >
  String cc; //current character received
  boolean newData = false; //flag that says we have a new data packet
  
  while(hc.available() > 0 && newData == false)
  {  
    cc = hc.read();
    Serial.print(cc);
    if(rxInProgress == true)
    {
      //add data to the message string if it isn't end marker
      if(cc != endMarker)
      {
        message += cc;
      }
      //if its end marker, finish the process
      else
      {
        rxInProgress = false;
        newData = true; //kills the loop
      }
    }
    else if(cc == startMarker)
    {
      rxInProgress = true;
    }
  }
  return message; //"" by default
}


String checkForInputSer()
{
  String message = "";
  if (Serial.available())
  {  
    while(Serial.available())
    {
      message += Serial.readString();
      Serial.println(message);
    }
  }
  Serial.println("done");
  return message;
}




/*****State functions ******/


//a loop based 2-relay temperature control system
void stateDualTempControl()
{
  measureTemperatures();
  calculateRelayStates();
  updateRelayStates();

  //uncomment for info on the usb serial line
  
  Serial.print("TC temps: ");
  Serial.print(temps[0]); Serial.print(", "); Serial.print(temps[1]);
  Serial.print("   TC CJ temps: ");
  Serial.print(cjTemps[0]); Serial.print(", "); Serial.print(cjTemps[1]);
  Serial.print("   Relay States: ");
  Serial.print(relayStates[0]); Serial.print(", "); Serial.println(relayStates[1]);  

  //create a function that fills a string buffer with your
  //chosen data format. 
  //string buf;
  //buf = formBluetoothPacket();
  //char bluetoothPacket[buf.length()];
  //buf.toCharArray(bluetoothPacket, buf.length());
  //hc.write(bluetoothPacket);
}

/* saving some work, not operational at the moment
void stateManualRelayControl()
{
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
*/

//this sends an in-sequence integer string delimited by commas
//to the bluetooth transmitter, the last value representing
//the sum. It then waits for a response on the receiver line from
//the raspberry pi that has another in-sequence integer string with 
//sum at the end. Both parties check the order of the sequence and that the
//sum is correct. Both parties know the start and end number. 
void stateChecksumMode()
{
  int n = 1000; //integers from 0 to n-1 are stringified and sent
  String messageOut = ""; //will be populated with the string data
  int checksum = 0; //sums integers included
  for(int i = 0; i < n; i++)
  {
    messageOut += String(i) + ",";
    checksum += i;
  }
  messageOut += String(checksum); //last element is a string
  messageOut += "\n\r"; //needed for serial parser on node-red side.
  
  //send this along the transmit line
  char bluetoothPacket[messageOut.length()]; //standard set of conversion operations
  messageOut.toCharArray(bluetoothPacket, messageOut.length());
  hc.write(bluetoothPacket);
  Serial.print("That message is ");
  Serial.print(sizeof(bluetoothPacket));
  Serial.println(" bytes");

  //seconds to wait for the RPi to respond with a similar message. 
  //penalty for a timeout is returning to the default state.
  int timeout = 60*1000; //milliseconds 
  double t0 = millis(); //present time
  double curtime = 0; //keeping track for timeout
  String messageBack; //message received from node-red, representing the sum it calculated

  //wait a few seconds for the RPi to process
  delay(5000);
  while(curtime < timeout)
  {
    messageBack = checkForInputBlue(); //checks input, "" if nothing
    if(messageBack == "")
    {
      delay(300);
    }
    else
    {
      break; //we got a m
    }
    curtime = millis() - t0;
  }

  //if the loop reached timeout
  if(messageBack == "")
  {
    Serial.println("Checksum mode reached a timeout, check the node-red receiver");
    return;
  }

  //put a stupid ending flag, just an extra "," at the end for the parsing code
  messageBack += ",";
  Serial.print("That message is  ");
  Serial.println(messageBack);

  

  //expected input from RPi: "0,1,2,3,4.....,sum". 
  //split the string, which is first converted to char array
  //to avoid dynamic memory allocation which is slow on arduino for large objects
  char pimess[messageBack.length()];
  messageBack.toCharArray(pimess, messageBack.length()); //the message in char[] format

  
  //here, n must be less than or equal to the number of numbers received in the message, otherwise
  //the teensy will freeze. put in a check here if you want better protection. 
  char *sumNums[n+1]; //n is defined above as the number of numbers in checksum packet, +1 is for the actual sum at the end
  char *ptr = NULL; //used in parsing function
  int index = 0;
  //get pointers to delimiter indices in the pimess message
  //allows for multiple delimiters, like ",:" will search for , and :
  ptr = strtok(pimess, ","); 
  //loop through expected delimiters
  while(ptr != NULL)
  {
    sumNums[index] = ptr;
    index++;
    ptr = strtok(NULL, ",");
  }

  //loop through all of the received numbers and calculate sum
  int newSum = 0;
  //also calculate the order
  bool outOfOrder = false;
  int outOfOrderIndex;
  int thenum; //temporary variable
  for(int i = 0; i < n; i++)
  {
    thenum = atoi(sumNums[i]);
    newSum += thenum;
    if(outOfOrder == false && i > 0 && (thenum != (atoi(sumNums[i-1]) + 1)))
    {
      outOfOrder = true;
      outOfOrderIndex = i;
      Serial.print("Got out of order on : " );
      Serial.println(outOfOrderIndex);
    }
  }

  //get the sum that the RPi sent, which is last element. 
  int sumfromnode = atoi(sumNums[n]);
  
  //if it is wrong, log the error somehow
  if(sumfromnode != newSum || outOfOrder)
  {
    communicationErrors += 1;
    Serial.print("Got a communication error: ");
    Serial.print(sumfromnode);
    Serial.print(" != ");
    Serial.print(newSum);
    Serial.print(", or outOfOrder = ");
    Serial.println(outOfOrder);
  }
  else
  {
    Serial.println("Got the correct checksum!");
  }
  
  return;
}


//this function allows you to use the USB serial input to 
//configure the HC06 bluetooth settings.
void stateBlueConfig()
{
  Serial.println("What would you like to configure about the HC06?");
  Serial.println("0 | HC06 baud");
  Serial.println("1 | Bluetooth name");
  Serial.println("2 | Pin code");
  Serial.println("3 | Exit, re-enter default state");
  Serial.print("> ");
  while(!Serial.available())
  {
  }
  String option = Serial.readString();
  option = option.trim();
  if(option == "0")
  {
    Serial.println("This function doesnt work for some reason. please debug before using");
    return;
    Serial.println("Choose a baud rate:");
    Serial.println("1 | 1200 bps");
    Serial.println("2 | 2400 bps");
    Serial.println("3 | 4800 bps");
    Serial.println("4 | 9600 bps");
    Serial.println("5 | 19200 bps");
    Serial.println("6 | 38400 bps");
    Serial.println("7 | 57600 bps");
    Serial.println("8 | 115200 bps");
    Serial.print("> ");
    while(!Serial.available())
    {
    }
    String bd = Serial.readString();
    bd = bd.trim();
    int int_bd = bd.toInt();
    if(int_bd <= 8 && int_bd >= 1)
    {
      char char_bd[1];
      itoa(int_bd, char_bd, 10); //10 is the base
      char cmd[10] = "AT+BAUD"; //command for baud rate setting
      cmd[7] = char_bd[0]; //add the number at the end
      cmd[8] = '\n'; //add newline at end as well
      cmd[9] = '\r';
      Serial.print("Debug, writing command: ");
      Serial.println(cmd);
      hc.write(cmd); //change baud rate on HC06
      delay(1000); 
      hc.end(); //end serial comms with HC06
      //the baud rate selected is 1200 * 2^(n - 1) where n is int_bd
      int real_bd = 1200 * pow(2, int_bd - 1);
      //UNLESS, option is 7 or 8, the scheme switches... stupidly
      if(int_bd == 7)
      {
        real_bd = 57600;
      }
      else if(int_bd == 8)
      {
        real_bd = 115200;
      }
      Serial.println("Debug, setting baud rate to");
      Serial.println(real_bd);
      hc.begin(real_bd); //set new baud rate
      delay(2000);
      return;
    }
    else
    {
      Serial.println("Not a correct option, must be 1 through 8. Try again");
      return;
    }
    
  }
  else if(option == "1")
  {
    Serial.println("Not coded yet, but slotted.");
    return;
  }
  else if(option == "2")
  {
    Serial.println("Not coded yet, but slotted.");
    return;
  }
  else if(option == "3")
  {
    state = "statechange: default";
    return;
  }
  else
  {
    Serial.print("Sorry, that option, ");
    Serial.print(option);
    Serial.println(", is not coded in yet. Try again");
    return; //no statechange, just re-enter this state.
  }
}

/*****Other Functions ******/


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

//calculates based on some algorithm (such as PID)
//what the relay states should be. presently not coded.
void calculateRelayStates()
{
  for(int i = 0; i < 2; i++)
  {
    relayStates[i] = 0;
  }
}
