/* FUNKY */
#ifdef  ARDUINO_AVR_LILYPAD_USB   //FUNKY
//#define SER1
#ifdef SER1
  #define SerialX Serial1
  #define BAUD_RATE 9600
#else
  #define SerialX Serial
  #define BAUD_RATE 57600
#endif
#define LED_PIN     13       // activity LED, comment out to disable

/* RFM2PI */
#elif ARDUINO_AVR_ATMEGA328_384_8  //RFM2PI
#define SerialX Serial
#define BAUD_RATE 38400
#define LED_PIN     9       // activity LED, comment out to disable

/*OTHER*/
#else  //OTHER
#define LED_PIN     13       // activity LED, comment out to disable
#define SerialX Serial
#define BAUD_RATE 57600
#endif

#include <RFM69.h>
#include <WirelessHEX69.h>
#include <SPI.h>
#include <util/crc16.h>
#include <avr/eeprom.h>

#define NUMBER_OF_RETRY 5 //number of retry for sendWithRetry
#define ACK_TIME    70  // # of ms to wait for an ack
#define TIMEOUT     3000 //for FOTA
#define VERSION "[RF69demo.13]"

#define RF69_EEPROM_ADDR ((uint8_t*) 0x80)


static byte bandToFreq (byte band) {
  return band == 4 ? RF69_433MHZ : band == 8 ? RF69_868MHZ : band == 9 ? RF69_915MHZ : 0;
}

const char INVALID1[] PROGMEM = "\rInvalid\n";
const char INITFAIL[] PROGMEM = "config save failed\n";

#define MAJOR_VERSION 0x00 // bump when EEPROM layout changes
// RF69 configuration area
typedef struct {
  byte nodeId;            // used by rf12_config, offset 0
  byte group;             // used by rf12_config, offset 1
  byte band;              //band
  byte format;            // used by rf12_config, offset 2
byte hex_output   :
  2;   // 0 = dec, 1 = hex, 2 = hex+ascii
byte collect_mode :
  1;   // 0 = ack, 1 = don't send acks
byte quiet_mode   :
  1;   // 0 = show all, 1 = show only valid packets
byte promiscuous_mode   :  
  1;   // 0 = disabled, 1 = enabled
byte encryption   :  
  1;   // 0 = disabled, 1 = enabled  
byte spare_flags  :
  2;
  word frequency_offset;  // used by rf12_config, offset 4
  char encryption_key[16]; // encrypt key 
  //byte pad[RF12_EEPROM_SIZE-8];
  word crc;
} 
RF69Config;

static RF69Config config = {10,250,RF69_868MHZ,0,0,0,0,0,0,0,1600,{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},0};


const char helpText1[] PROGMEM =
"\n"
"Available commands:\n"
"  <nn> i     - set node ID (standard node ids are 1..30)\n"
"  <n> b      - set MHz band (4 = 433, 8 = 868, 9 = 915)\n"
"  <nnnn> o   - change frequency offset within the band (default 1600)\n"
"               96..3903 is the range supported by the RFM12B\n"
"  <nnn> g    - set network group (RFM12 only allows 212, 0 = any)\n"
"  <n> c      - set collect mode (advanced, normally 0)\n"
"  t          - broadcast max-size test packet, request ack\n"
"  l          - show led activity\n"
"  ...,<nn> a - send data packet to node <nn>, request ack\n"
"  ...,<nn> s - send data packet to node <nn>, no ack\n"
"  <n> q      - set quiet mode (1 = don't report bad packets)\n"
"  <n> x      - set reporting format (0: decimal, 1: hex, 2: hex+ascii)\n"
"  <nnn> y    - enable signal strength trace mode, default:0 (disabled)\n"
"               sample interval <nnn> secs/100 (0.01s-2.5s) eg 10y=0.1s\n"
"  <nn> j     - sends local temperatur to node j\n"
"  u          - upload firmware using FOTA/Wireless programming\n"
"  m          - dumps registers\n"
"  ... n      - set encryption key, consists of 16 numbers, \n"
"             - if less than 16 number provided they are padded by zeros\n"
"             - 0n mean disable encryption\n" 
"             - 1,2,3n -> 1,2,3,0,0,0,0,0,0,0,0,0,0,0,0,0\n"
"  <n> p      - promiscuite mode 0 off, 1 on\n"
"  123 z      - total power down, needs a reset to start up again\n"
/*
"Remote control commands:\n"
"  <hchi>,<hclo>,<addr>,<cmd> f     - FS20 command (868 MHz)\n"
"  <addr>,<dev>,<on> k              - KAKU command (433 MHz)\n"
*/
;

static void printOneChar (char c) {
  SerialX.print(c);
}

static void showNibble (byte nibble) {
  char c = '0' + (nibble & 0x0F);
  if (c > '9')
    c += 7;
  SerialX.print(c);
}

static void showByte (byte value) {
  if (config.hex_output) {
    showNibble(value >> 4);
    showNibble(value);
  } 
  else
    SerialX.print((word) value);
}


static void showString (PGM_P s) {
  while (s != NULL) {
    char c = pgm_read_byte(s++);
    if (c == 0)
      break;
    if (c == '\n')
      printOneChar('\r');
    printOneChar(c);
  }
}

static void showStringln(PGM_P s) {
  showString(s);
  printOneChar('\r');  
}


static void displayVersion () {
  showString(PSTR(VERSION));
}

static void configDump() {  
  //showStringln(PSTR(" O i15 g3 @ 868 MHz q1"));
//    uint8_t nodeId = eeprom_read_byte(RF12_EEPROM_ADDR);
//    uint8_t flags = eeprom_read_byte(RF12_EEPROM_ADDR + 3);
//    uint16_t freq = eeprom_read_word((uint16_t*) (RF12_EEPROM_ADDR + 4));

    uint8_t nodeId = config.nodeId;
    uint16_t freq = config.frequency_offset;

    // " A i1 g178 @ 868 MHz "
    SerialX.print(' ');
    SerialX.print((char) ('@' + (nodeId & 0x1F)));
    SerialX.print(" i");
    SerialX.print(nodeId);
    if (config.collect_mode)
        SerialX.print('*');
    SerialX.print(" g");
    SerialX.print(config.group);
    SerialX.print(" @ ");
    uint8_t band = config.band;
    SerialX.print(band == RF69_433MHZ ? 433 :
                 band == RF69_868MHZ ? 868 :
                 band == RF69_915MHZ ? 915 : 0);
    SerialX.print(" MHz");
    if (config.collect_mode) {
        SerialX.print(" c1");
    }
    if (freq != 1600) {
        SerialX.print(" o");
        SerialX.print(freq);
    }
    if (config.quiet_mode) {
        SerialX.print(" q1");
    }
    if (config.hex_output) {
        SerialX.print(" x");
        SerialX.print(config.hex_output);
    }
    if (config.promiscuous_mode) {
        SerialX.print(" p");
        SerialX.print(config.promiscuous_mode);          
    }
    if (config.encryption) {
        SerialX.print(" e");
        SerialX.print(config.encryption);          
    }

    SerialX.println();
}



static void showHelp () {
  showString(helpText1);
  showString(PSTR("Current configuration:\n"));
  configDump();
}

static char cmd;
static word value;
static byte stack[RF69_MAX_DATA_LEN+4], top, sendLen, dest;
static byte testCounter;
byte trace_mode = 0;
RFM69 radio;

#define DBG(m) {SerialX.print("! "); SerialX.print(__FUNCTION__); SerialX.print(':'); SerialX.print(__LINE__); SerialX.print(' ');  showStringln(PSTR(m));}
static boolean configSilent() {  
  if (config.format == MAJOR_VERSION) {
  
    radio.initialize(config.band, config.nodeId, config.group);
    radio.promiscuous(config.promiscuous_mode);
    
    if (config.encryption) {
      radio.encrypt(config.encryption_key); 
    } else {
      radio.encrypt(0); 
    }
    return true;
  } else {
    return false;
  }  
}

static word calcCrc (const void* ptr, byte len) {
    word crc = ~0;
    for (byte i = 0; i < len; ++i)
        crc = _crc16_update(crc, ((const byte*) ptr)[i]);
    return crc;
}

static void saveConfig () {
    config.format = MAJOR_VERSION;
    config.crc = calcCrc(&config, sizeof config - 2);

    // eeprom_write_block(&config, RF12_EEPROM_ADDR, sizeof config);
    // this uses 170 bytes less flash than eeprom_write_block(), no idea why
    eeprom_write_byte(RF69_EEPROM_ADDR, ((byte*) &config)[0]);
    for (byte i = 0; i < sizeof config; ++ i)
        eeprom_write_byte(RF69_EEPROM_ADDR + i, ((byte*) &config)[i]);

  if (configSilent())
    configDump();
  else
    showString(INITFAIL);  
};

static boolean loadConfig () {
      // eeprom_read_block(&config, RF12_EEPROM_ADDR, sizeof config);
    // this uses 166 bytes less flash than eeprom_read_block(), no idea why
    for (byte i = 0; i < sizeof config; ++ i)
        ((byte*) &config)[i] = eeprom_read_byte(RF69_EEPROM_ADDR + i);

    return ((config.format == MAJOR_VERSION) && (config.crc == calcCrc(&config, sizeof config - 2)));
}


static void activityLed (byte on) {
#ifdef LED_PIN
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, on);
#endif
}

char input[64]; //serial input buffer
byte targetID=0;

static void wirelessprogramming() {
  while(1) {
    byte inputLen = readSerialLine(input, 10, 64, 100);
    
    if (inputLen==4 && input[0]=='F' && input[1]=='L' && input[2]=='X' && input[3]=='?') {
      if (targetID==0)
        SerialX.println("TO?");
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
        SerialX.print("TO:");
        SerialX.print(newTarget);
        SerialX.println(":OK");
      }
      else
      {
        SerialX.print(input);
        SerialX.print(":INV");
        return;
      }
    }
  }
}
static void handleInput (char c) {
  if ('0' <= c && c <= '9') {
    value = 10 * value + c - '0';
    return;
  }

  if (c == ',') {
    if (top < sizeof stack)
      stack[top++] = value; // truncated to 8 bits
    value = 0;
    return;
  }

  if ('a' <= c && c <= 'z') {
    showString(PSTR("> "));
    for (byte i = 0; i < top; ++i) {
      SerialX.print((word) stack[i]);
      printOneChar(',');
    }
    SerialX.print(value);
    SerialX.println(c);
  }

  // keeping this out of the switch reduces code size (smaller branch table)
  if (c == '>') {
    DBG(">");
    /*
    // special case, send to specific band and group, and don't echo cmd
    // input: band,group,node,header,data...
    stack[top++] = value;
    // TODO: frequency offset is taken from global config, is that ok?

    
    rf12_initialize(stack[2], bandToFreq(stack[0]), stack[1],
    config.frequency_offset);
    rf12_sendNow(stack[3], stack + 4, top - 4);
    rf12_sendWait(2);
    rf12_configSilent();
    */
  } 
  else if (c > ' ') {
    switch (c) {

    case 'i': // set node id
    //TODO test node range
      if ((value >= 0) && (value <= 255)) {
        config.nodeId = value;
        saveConfig();
        }
      break;

    case 'b': // set band: 4 = 433, 8 = 868, 9 = 915
      value = bandToFreq(value);
      if (value) {
        config.band = value;
        config.frequency_offset = 1600;
        saveConfig();
      }
      break;

    case 'm':
        //radio.readAllRegs();
        {
          uint8_t regVal;

          showStringln(PSTR("Address - HEX - BIN"));
          for (uint8_t regAddr = 1; regAddr <= 0x4F; regAddr++)
          {
            regVal=radio.readReg(regAddr);
            SerialX.print(regAddr, HEX);
            SerialX.print(" - ");
            SerialX.print(regVal,HEX);
            SerialX.print(" - ");
            SerialX.println(regVal,BIN);
          }
        }
        break;
    case 'o': 
      { // Increment frequency within band
        // Stay within your country's ISM spectrum management guidelines, i.e.
        // allowable frequencies and their use when selecting operating frequencies.
        if ((value > 95) && (value < 3904)) { // supported by RFM12B
          config.frequency_offset = value;
          saveConfig();
        }

        // this code adds about 400 bytes to flash memory use
        // display the exact frequency associated with this setting
        //TODO ?????
        byte freq = config.band;        
        uint32_t f1 = freq * 100000L + config.band * 25L * config.frequency_offset;
        SerialX.print((word) (f1 / 10000));
        printOneChar('.');
        word f2 = f1 % 10000;
        // tedious, but this avoids introducing floating point
        printOneChar('0' + f2 / 1000);
        printOneChar('0' + (f2 / 100) % 10);
        printOneChar('0' + (f2 / 10) % 10);
        printOneChar('0' + f2 % 10);
        SerialX.println(" MHz");
        break;
      }

    case 'g': // set network group
      config.group = value;
      saveConfig();
      break;

    case 'c': // set collect mode (off = 0, on = 1)
      config.collect_mode = value;
      saveConfig();
      break;

    case 'j': //send temperature
      {      
        showString(PSTR("Sending temperatur: "));
        byte temperatur = radio.readTemperature(-1);
        showByte(temperatur);
        showString(PSTR("C to node: "));
        showByte(value);              

        sendLen = 2;
        dest = value;
        stack[0] = temperatur;
        stack[1] = testCounter++;
        cmd = 'a';
      }
      break;

    case 'p': //set promiscuout mode
      config.promiscuous_mode=value!=0;
      showString(PSTR("Setting promiscuite mode: "));            
      SerialX.println(config.promiscuous_mode);
      radio.promiscuous(config.promiscuous_mode);
      saveConfig();
      break;
      
    case 't': // broadcast a maximum size test packet, request an ack      
      sendLen = ((value==0)?RF69_MAX_DATA_LEN:min(value,RF69_MAX_DATA_LEN));
      dest = (top==0)?RF69_BROADCAST_ADDR:stack[0];
      cmd = (dest == RF69_BROADCAST_ADDR)? 's':'a';
      for (byte i = 0; i < sendLen; ++i)
        stack[i] = i + testCounter;
      showString(PSTR("test "));
      showByte(testCounter); // first byte in test buffer
      ++testCounter;
      break;

    case 'n':  //set enscryption key
      if (top == 0 && value==0) {
        //disable encryption
        config.encryption = 0;
      } else {
        config.encryption = 1;
        stack[top++]=value;
        memset(&config.encryption_key, 0, sizeof(config.encryption_key));
        memcpy(&config.encryption_key, stack, min(top,sizeof(config.encryption_key)));
      }
      SerialX.println(sizeof(config.encryption_key));
      SerialX.println(top);
      showString(PSTR("Current Key: "));   
      for(value=0; value<sizeof(config.encryption_key);value++) {
        showByte(config.encryption_key[value]); 
        printOneChar(' ');
      }      
      SerialX.println();
      showString(PSTR("Encryption: "));   
      showStringln((config.encryption)?PSTR("Enabled"):PSTR("Disabled"));
      saveConfig();
      break;
    case 'a': // send packet to node ID N, request an ack
    case 's': // send packet to node ID N, no ack
      cmd = c;
      sendLen = top;
      dest = value;
      break;

    case 'u': //upload firmware
      wirelessprogramming();
      break; 
    case 'f': // send FS20 command: <hchi>,<hclo>,<addr>,<cmd>f
        //TODO
      DBG("f not implemented");

/*
      rf12_initialize(0, RF12_868MHZ, 0);
      activityLed(1);
      fs20cmd(256 * stack[0] + stack[1], stack[2], value);
      activityLed(0);
      rf12_configSilent();
      */
      break;

    case 'k': // send KAKU command: <addr>,<dev>,<on>k
      //TODO
      DBG("k not implemented");
    /*
      rf12_initialize(0, RF12_433MHZ, 0);
      activityLed(1);
      kakuSend(stack[0], stack[1], value);
      activityLed(0);
      rf12_configSilent();
      */
      break;

    case 'z': // put the ATmega in ultra-low power mode (reset needed)
      if (value == 123) {
        //TODO
        DBG("z not implemented");
/*
        showString(PSTR(" Zzz...\n"));
        Serial.flush();
        rf12_sleep(RF12_SLEEP);
        cli();
        Sleepy::powerDown();
        */
      }
      break;

    case 'q': // turn quiet mode on or off (don't report bad packets)
      config.quiet_mode = value;
      saveConfig();
      break;

    case 'x': // set reporting mode to decimal (0), hex (1), hex+ascii (2)
      config.hex_output = value;
      saveConfig();
      break;

    case 'v': //display the interpreter version and configuration
      displayVersion();
      configDump();
      break;

      // the following commands all get optimised away when TINY is set

    case 'l': // turn activity LED on or off
      activityLed(value);
      break;

    case 'd': // dump all log markers
        //TODO
        DBG("d not implemented");
/*    
      if (df_present())
        df_dump();
        */
      break;

    case 'r': // replay from specified seqnum/time marker
        //TODO
        DBG("r not implemented");
/*    
      if (df_present()) {
        word seqnum = (stack[0] << 8) | stack[1];
        long asof = (stack[2] << 8) | stack[3];
        asof = (asof << 16) | ((stack[4] << 8) | value);
        df_replay(seqnum, asof);
      }
      */
      break;

    case 'e': // erase specified 4Kb block
        //TODO
      DBG("e not implemented");
/*    
      if (df_present() && stack[0] == 123) {
        word block = (stack[1] << 8) | value;
        df_erase(block);
      }
      */
      break;

    case 'w': // wipe entire flash memory
        //TODO
      DBG("w not implemented");
/*
      if (df_present() && stack[0] == 12 && value == 34) {
        df_wipe();
        showString(PSTR("erased\n"));
      }
      */
      break;

    case 'y': // turn signal strength trace mode on or off (rfm69 only)
      trace_mode = value;
      break;

    default:
      showHelp();
    }
  }

  value = top = 0;
}

static void displayASCII (const byte* data, byte count) {
  for (byte i = 0; i < count; ++i) {
    printOneChar(' ');
    char c = (char) data[i];
    printOneChar(c < ' ' || c > '~' ? '.' : c);
  }
  Serial.println();
}



void setup() {
  SerialX.begin(BAUD_RATE);

  activityLed(1);
  delay(100); // shortened for now. Handy with JeeNode Micro V1 where ISP
  // interaction can be upset by RF12B startup process.

  displayVersion();

  pinMode(4,OUTPUT);
  digitalWrite(4,LOW);
  delay(100);
  radio.setCS(10);
  
  if (loadConfig()) {
    configSilent();
  } 
  else {
    memset(&config, 0, sizeof config);
    config.nodeId = 0x4F;                           // RFM12Pi - 433 MHz, node 15
    config.group = 0xD2;                           // RFM12Pi - default group 210
    config.band=RF69_868MHZ;
    config.frequency_offset = 1600;
    config.quiet_mode = true;   // Default flags, quiet on
    saveConfig();
  }
  

  
#ifdef IS_RFM69HW
  radio.setHighPower(); //only for RFM69HW!
#endif


  configDump();  
  showHelp();

  delay(1000);        //rfm12pi keep LED for for 1s to show it's working at startup 
  activityLed(0);
 
}

void loop() {
  if (SerialX.available())
    handleInput(SerialX.read());
  if (trace_mode == 0) {
    if (radio.receiveDone()) {
      byte n = radio.DATALEN;
      //DBG("no CRC equivavlent");
      if (/*rf12_crc == 0*/ 1)
      {
        activityLed(1);
        showString(PSTR("OK"));
      }
      else {
        if (config.quiet_mode)
          return;
        showString(PSTR(" ?"));
        if (n > 20) // print at most 20 bytes if crc is wrong
          n = 20;
      }
      if (config.hex_output)
        printOneChar('X');
      if (config.group == 0) {
        showString(PSTR(" G"));
        showByte(config.group);
      }
      printOneChar(' ');
      showByte(radio.SENDERID);
      for (byte i = 0; i < n; ++i) {
        if (!config.hex_output)
          printOneChar(' ');
        showByte(radio.DATA[i]);
      }
  
      // display RSSI value after packet data
      showString(PSTR(" ("));
      if (config.hex_output)
        //TODO modified RSSI 16bit vs 8 bit
        showByte(radio.RSSI>>8);
      else
        SerialX.print(-(radio.RSSI>>1));
      showString(PSTR(") "));

      //disply to address in promiscuite mode
      if (config.promiscuous_mode) {
        printOneChar(' ');
        showByte(radio.SENDERID);
        showString(PSTR("->"));
        showByte(radio.TARGETID);
      }
      SerialX.println();
  
      if (config.hex_output > 1) { // also print a line as ascii
        showString(PSTR("ASC "));
        if (config.group == 0) {
          showString(PSTR(" II "));
        }
        /*TODO oroginal code wasable to */
        //printOneChar(rf12_hdr & RF12_HDR_DST ? '>' : '<');
        //show sign for incomming request
        printOneChar('<');
        //printOneChar('@' + (rf12_hdr & RF12_HDR_MASK));
        //only 32 notes is valid for RFM12 ,however RFM69 allows 255  , shown only first 32 : in ascii mode
        printOneChar('@' + (radio.SENDERID & 0x1F));
        displayASCII((const byte*) radio.DATA, n);
      }
  
      if (/*rf12_crc == 0*/ 1) {
        activityLed(1);
  
        if (radio.ACKRequested() && (config.collect_mode == 0)) {
          if (radio.TARGETID == config.nodeId) {          
            showString(PSTR(" -> ack\n"));
            radio.sendACK();
          } else {
            showString(PSTR(" -> ack not sent\n"));
          }
          
        }
        activityLed(0);
      }
    }
  
    if (cmd && radio.canSend()) {
      activityLed(1);
  
      showString(PSTR(" -> "));
      SerialX.print((word) sendLen);
      showString(PSTR(" b "));
      if (cmd == 'a') {
        if (radio.sendWithRetry(dest, stack, sendLen, NUMBER_OF_RETRY ,ACK_TIME)) {
          showString(PSTR(" ACK recieved"));
        } else {
          showString(PSTR(" ACK not recieved"));
        }
      } else {
        radio.send(dest, stack, sendLen);
        showString(PSTR(" ACK not required"));
      }
      showString(PSTR("\n"));
      cmd = 0;
  
      activityLed(0);
    }
    activityLed(0);

  } else {
      //DBG("Trace mode not working");      
      byte y = radio.readRSSI(true);
      for (byte i = 0; i < (100-y); ++i) {
          printOneChar('-');
      }
      SerialX.print("*");
      for (byte i = 0; i < (y); ++i) {
          printOneChar(' ');
      }
      SerialX.print(-y);
      SerialX.println("dB");
      
      delay(trace_mode*10);
  }   
}
