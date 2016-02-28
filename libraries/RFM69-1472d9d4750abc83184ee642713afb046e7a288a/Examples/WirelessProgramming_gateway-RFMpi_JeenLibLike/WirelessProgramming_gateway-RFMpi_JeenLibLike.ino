// **********************************************************************************
// This sketch is an example of how wireless programming can be achieved with a Moteino
// that was loaded with a custom 1k bootloader (DualOptiboot) that is capable of loading
// a new sketch from an external SPI flash chip
// This is the GATEWAY node, it does not need a custom Optiboot nor any external FLASH memory chip
// (ONLY the target node will need those)
// The sketch includes logic to receive the new sketch from the serial port (from a host computer) and 
// transmit it wirelessly to the target node
// The handshake protocol that receives the sketch from the serial port 
// is handled by the SPIFLash/WirelessHEX69 library, which also relies on the RFM69 library
// These libraries and custom 1k Optiboot bootloader for the target node are at: http://github.com/lowpowerlab
// **********************************************************************************
// Copyright Felix Rusu, LowPowerLab.com
// Library and code by Felix Rusu - felix@lowpowerlab.com
// Added changes for Funky V3 by Vitek VLCEK Copyright Vitek VLCEK
// **********************************************************************************
// License
// **********************************************************************************
// This program is free software; you can redistribute it 
// and/or modify it under the terms of the GNU General    
// Public License as published by the Free Software       
// Foundation; either version 3 of the License, or        
// (at your option) any later version.                    
//                                                        
// This program is distributed in the hope that it will   
// be useful, but WITHOUT ANY WARRANTY; without even the  
// implied warranty of MERCHANTABILITY or FITNESS FOR A   
// PARTICULAR PURPOSE. See the GNU General Public        
// License for more details.                              
//                                                        
// You should have received a copy of the GNU General    
// Public License along with this program.
// If not, see <http://www.gnu.org/licenses/>.
//                                                        
// Licence can be viewed at                               
// http://www.gnu.org/licenses/gpl-3.0.txt
//
// Please maintain this license information along with authorship
// and copyright notices in any redistribution of this code
// **********************************************************************************
#include <RFM69.h>          //get it here: https://www.github.com/lowpowerlab/rfm69
#include <SPI.h>
#include <WirelessHEX69.h> //get it here: https://github.com/LowPowerLab/WirelessProgramming/tree/master/WirelessHEX69

#define NODEID             253  //this node's ID, should be unique among nodes on this NETWORKID
#define NETWORKID          250  //what network this node is on
//Match frequency to the hardware version of the radio on your Moteino (uncomment one):
//#define FREQUENCY   RF69_433MHZ
#define FREQUENCY   RF69_868MHZ
//#define FREQUENCY     RF69_915MHZ
#define ENCRYPTKEY "sampleEncryptKey" //(16 bytes of your choice - keep the same on all encrypted nodes)
//#define IS_RFM69HW             //uncomment only for RFM69HW! Leave out if you have RFM69W!

#define SERIAL_BAUD 38400
#define ACK_TIME    50  // # of ms to wait for an ack
#define TIMEOUT     3000

#define LED            13// Moteinos hsave LEDs on D9


RFM69 radio;
char c = 0;
char input[64]; //serial input buffer
byte targetID=0;

void setup(){
  Serial.begin(SERIAL_BAUD);

  pinMode(4,OUTPUT);
  digitalWrite(4,LOW);
  delay(100);
  radio.setCS(10);
  
  radio.initialize(FREQUENCY,NODEID,NETWORKID);
  radio.encrypt(ENCRYPTKEY); //OPTIONAL
#ifdef IS_RFM69HW
  radio.setHighPower(); //only for RFM69HW!
#endif
  while (!Serial) {Blink(LED,500); /*heartbeat*/};
  Serial.println("Start wireless gateway...");
}

void wirelessprogrammingmode() {
  while(1) {
    byte inputLen = readSerialLine(input, 10, 64, 100); //readSerialLine(char* input, char endOfLineChar=10, byte maxLength=64, uint16_t timeout=1000);
    
    if (inputLen==4 && input[0]=='F' && input[1]=='L' && input[2]=='X' && input[3]=='?') {
      if (targetID==0)
        Serial.println("TO?");
      else
        CheckForSerialHEX((byte*)input, inputLen, radio, targetID, TIMEOUT, ACK_TIME, false);
        return;
    }
    else if (inputLen>3 && inputLen<=6 && input[0]=='T' && input[1]=='O' && input[2]==':')
    {
      byte newTarget=0;
      for (byte i = 3; i<inputLen; i++) //up to 3 characters for target ID
        if (input[i] >=48 && input[i]<=57)
          newTarget = newTarget*10+input[i]-48;
        else
        {
          newTarget=0;
          break;
        }
      if (newTarget>0)
      {
        targetID = newTarget;
        Serial.print("TO:");
        Serial.print(newTarget);
        Serial.println(":OK");
      }
      else
      {
        Serial.print(input);
        Serial.print(":INV");
        return;
      }
    }
  }
}


static void showNibble (byte nibble) {
  char c = '0' + (nibble & 0x0F);
  if (c > '9')
    c += 7;
  Serial.print(c);
}
int useHex = 0;
static void showByte (byte value) {
  if (useHex) {
    showNibble(value >> 4);
    showNibble(value);
  } else
    Serial.print((int) value);
}
        
static void addCh (char* msg, char c) {
  byte n = strlen(msg);
  msg[n] = c;
}

static void addInt (char* msg, word v) {
  if (v >= 10)
    addInt(msg, v / 10);
  addCh(msg, '0' + v % 10);
}

#define RF12_EEPROM_SIZE 64
typedef struct {
  byte nodeId;
  byte group;
  char msg[RF12_EEPROM_SIZE-4];
  word crc;
} RF12Config;
  
static RF12Config config;




void loop(){
  //Serial.println("w - wireless mode");
  if (Serial.available()) {
    if (Serial.read()=='w') {
      wirelessprogrammingmode();
    }
  }
  
  if (radio.receiveDone())
  {

/*
    if (rf12_crc!=0)
        return;
    activityLed(1);
*/    
    if (useHex)
      Serial.print('X');
    if (config.group == 0) {
      Serial.print(" G");
      showByte(NETWORKID);
    }
    Serial.print(' ');
    //showByte(rf12_hdr);
    showByte(0);
    for (byte i = 0; i < radio.DATALEN; ++i) {
      if (!useHex)
        Serial.print(' ');
      showByte(radio.DATA[i]);
    }
    Serial.println();

    //if (rf12_crc == 0) {
    //  activityLed(1);

/*      if (df_present())
        df_append((const char*) rf12_data - 2, rf12_len + 2);

      if (RF12_WANTS_ACK && (config.nodeId & COLLECT) == 0) {
        Serial.println(" -> ack");
        rf12_sendStart(RF12_ACK_REPLY, 0, 0);
      }

      activityLed(0);
   // }
  }

*/
    
    
    if (radio.ACK_REQUESTED)
    {
      radio.sendACK();
      Serial.print(" -> ack");
    }
    
    Serial.println();
  }
  Blink(LED,5); //heartbeat
}

void Blink(byte PIN, int DELAY_MS)
{
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN,HIGH);
  delay(DELAY_MS);
  digitalWrite(PIN,LOW);
}
