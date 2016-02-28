// **********************************************************************************
// This sketch is an example of how wireless programming can be achieved with a Moteino
// that was loaded with a custom 1k bootloader (DualOptiboot) that is capable of loading
// a new sketch from an external SPI flash chip
// The sketch includes logic to receive the new sketch 'over-the-air' and store it in
// the FLASH chip, then restart the Moteino so the bootloader can continue the job of
// actually reflashing the internal flash memory from the external FLASH memory chip flash image
// The handshake protocol that receives the sketch wirelessly by means of the RFM69 radio
// is handled by the SPIFLash/WirelessHEX69 library, which also relies on the RFM69 library
// These libraries and custom 1k Optiboot bootloader are at: http://github.com/lowpowerlab
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
#define DEBUG
#include <RFM69.h>         //get it here: https://www.github.com/lowpowerlab/rfm69
#include <SPI.h>
//#include <SPIFlash.h>      //get it here: https://www.github.com/lowpowerlab/spiflash
#include <avr/wdt.h>
#include <WirelessHEX69.h> //get it here: https://github.com/LowPowerLab/WirelessProgramming/tree/master/WirelessHEX69

#define NODEID      123       // node ID used for this unit
#define GATEWAYID   254       //id of gateway
#define NETWORKID   250
//Match frequency to the hardware version of the radio on your Moteino (uncomment one):
//#define FREQUENCY   RF69_433MHZ
#define FREQUENCY   RF69_868MHZ
//#define FREQUENCY     RF69_915MHZ
//#define IS_RFM69HW  //uncomment only for RFM69HW! Leave out if you have RFM69W!
#define SerialX Serial1
#define SERIAL_BAUD 9600
#define ACK_TIME    30  // # of ms to wait for an ack
#define ENCRYPTKEY "sampleEncryptKey" //(16 bytes of your choice - keep the same on all encrypted nodes)
#define BLINKPERIOD 500

#ifdef LED
#undef LED
#endif

#ifdef __AVR_ATmega1284P__
  #define LED           15 // Moteino MEGAs have LEDs on D15
  #define FLASH_SS      23 // and FLASH SS on D23
#elif defined(__AVR_ATmega32U4__)
  #define LED           8 // Moteinos hsave LEDs on D9
  #define FLASH_SS      8 // and FLASH SS on D8
#else
  #define LED           9 // Moteinos hsave LEDs on D9
  #define FLASH_SS      8 // and FLASH SS on D8
#endif

RFM69 radio;
char input = 0;
long lastPeriod = -1;

/////////////////////////////////////////////////////////////////////////////
// flash(SPI_CS, MANUFACTURER_ID)
// SPI_CS          - CS pin attached to SPI flash chip (8 inR case of Moteino)
// MANUFACTURER_ID - OPTIONAL, 0x1F44 for adesto(ex atmel) 4mbit flash
//                             0xEF30 for windbond 4mbit flash
//                             0xEF40 for windbond 16/64mbit flash
/////////////////////////////////////////////////////////////////////////////
//SPIFlash flash(FLASH_SS, 0xEF30); //EF30 for windbond 4mbit flash

void setup(){
  pinMode(LED, OUTPUT);
  SerialX.begin(SERIAL_BAUD);

  pinMode(4,OUTPUT);
  digitalWrite(4,LOW);
  delay(100);
  radio.setCS(10);
  
  radio.initialize(FREQUENCY,NODEID,NETWORKID);
  radio.encrypt(ENCRYPTKEY); //OPTIONAL
#ifdef IS_RFM69HW
  radio.setHighPower(); //only for RFM69HW!
#endif
  //while (!SerialX) {Blink(LED,500); /*heartbeat*/};
  SerialX.println("Start");
/*
  if (flash.initialize())
    SerialX.println("SPI Flash Init OK!");
  else
    SerialX.println("SPI Flash Init FAIL!");
    */
}

typedef struct {
  int           nodeId; //store this nodeId
  unsigned long uptime; //uptime in ms
  float         temp;   //temperature maybe?
} Payload;
Payload theData;


void Blink(byte PIN, int DELAY_MS)
{
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN,HIGH);
  delay(DELAY_MS);
  digitalWrite(PIN,LOW);
}

#define NEW_FLASH_OFFSET 0x3800
#define SIZE_OF_NEW_FLASH_HEADER 2

void printHex(int a){
  
       char tmp[5];
       sprintf(tmp, "%.4X",a); 
       SerialX.print(tmp);
}

void loop(){
  // This part is optional, useful for some debugging.
  // Handle serial input (to allow basic DEBUGGING of FLASH chip)
  // ie: display first 256 bytes in FLASH, erase chip, write bytes at first 10 positions, etc
  if (SerialX.available() > 0) {
    input = SerialX.read();
    if (input == 'd') //d=dump first page
    {
      for(int base=0;base<NEW_FLASH_OFFSET+SPM_PAGESIZE;base += (base==0)?NEW_FLASH_OFFSET:(SPM_PAGESIZE-16)){
        SerialX.print("Flash: 0x");
        printHex(base);SerialX.println();
        
        for(int counter=0;counter<60;) {
          printHex(pgm_read_word(counter +base));
          counter+=2;
          SerialX.print('.');
          if (counter % 0x10 ==0) SerialX.println();
        }
        
        SerialX.println();
      }
    }
    /*else if (input == 'e')
    {
      SerialX.print("Erasing Flash chip ... ");
      //flash.chipErase();
      //while(flash.busy());
      SerialX.println("DONE");
    }
    else if (input == 'i')
    {
      SerialX.print("DeviceID: ");
      
    }*/
    else if (input == 'r')
    {
      SerialX.print("Rebooting");
      resetUsingWatchdog(true);
    }
    else if (input == 'R')
    {
      SerialX.print("RFM69regs:");
      radio.readAllRegs();
    }
    /*else if (input >= 48 && input <= 57) //0-9
    {
      SerialX.print("\nWriteByte("); SerialX.print(input); SerialX.print(")");
      //flash.writeByte(input-48, millis()%2 ? 0xaa : 0xbb);
    }*/
  }
  
  // Check for existing RF data, potentially for a new sketch wireless upload
  // For this to work this check has to be done often enough to be
  // picked up when a GATEWAY is trying hard to reach this node for a new sketch wireless upload
  if (radio.receiveDone())
  {
    SerialX.print("Got [");
    SerialX.print(radio.SENDERID);
    SerialX.print(':');
    SerialX.print(radio.DATALEN);
    SerialX.print("] > ");
    for (byte i = 0; i < radio.DATALEN; i++)
      SerialX.print((char)radio.DATA[i], HEX);
    SerialX.println();
    CheckForWirelessHEX(radio, NULL, true);
    SerialX.println();
  }

  //else SerialX.print('.');
  
  ////////////////////////////////////////////////////////////////////////////////////////////
  // Real sketch code here, let's blink the onboard LED
  if ((int)(millis()/BLINKPERIOD) > lastPeriod)
  {
    lastPeriod++;
    digitalWrite(LED, lastPeriod%2);

    //fill in the struct with new values
    theData.nodeId = NODEID;
    theData.uptime = millis();
    theData.temp = 91.23; //it's hot!
    
    SerialX.print("Sending ");
    SerialX.print(sizeof(theData));
    SerialX.print("B");
    if (radio.sendWithRetry(GATEWAYID, (const void*)(&theData), sizeof(theData)))
      SerialX.print(" OK");
    else SerialX.print(" ERR");
    SerialX.println();
    Blink(LED,3);
  }
  ////////////////////////////////////////////////////////////////////////////////////////////
}
