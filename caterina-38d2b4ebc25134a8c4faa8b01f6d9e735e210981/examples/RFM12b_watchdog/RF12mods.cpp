/// @file
/// RFM12B driver implementation
// 2009-02-09 <jc@wippler.nl> http://opensource.org/licenses/mit-license.php

#include "RF12mods.h"
#include <avr/io.h>
#include <util/crc16.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>
#if ARDUINO >= 100
#include <Arduino.h> // Arduino 1.0
#else
#include <WProgram.h> // Arduino 0022
#endif

// pin change interrupts are currently only supported on ATmega328's
// #define PINCHG_IRQ 1    // uncomment this to use pin-change interrupts

// maximum transmit / receive buffer: 3 header + data + 2 crc bytes
#define RF_MAX   (RF12_MAXDATA + 5)

// pins used for the RFM12B interface - yes, there *is* logic in this madness:
//
//  - leave RFM_IRQ set to the pin which corresponds with INT0, because the
//    current driver code will use attachInterrupt() to hook into that
//  - (new) you can now change RFM_IRQ, if you also enable PINCHG_IRQ - this
//    will switch to pin change interrupts instead of attach/detachInterrupt()
//  - use SS_DDR, SS_PORT, and SS_BIT to define the pin you will be using as
//    select pin for the RFM12B (you're free to set them to anything you like)
//  - please leave SPI_SS, SPI_MOSI, SPI_MISO, and SPI_SCK as is, i.e. pointing
//    to the hardware-supported SPI pins on the ATmega, *including* SPI_SS !

#if defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)

#define RFM_IRQ     2
#define SS_DDR      DDRB
#define SS_PORT     PORTB
#define SS_BIT      0

#define SPI_SS      53    // PB0, pin 19
#define SPI_MOSI    51    // PB2, pin 21
#define SPI_MISO    50    // PB3, pin 22
#define SPI_SCK     52    // PB1, pin 20

#elif defined(__AVR_ATmega644P__)

#define RFM_IRQ     10
#define SS_DDR      DDRB
#define SS_PORT     PORTB
#define SS_BIT      4

#define SPI_SS      4
#define SPI_MOSI    5
#define SPI_MISO    6
#define SPI_SCK     7

#elif defined(__AVR_ATtiny84__) || defined(__AVR_ATtiny44__)

#define RFM_IRQ     2
#define SS_DDR      DDRB
#define SS_PORT     PORTB
#define SS_BIT      1

#define SPI_SS      1     // PB1, pin 3
#define SPI_MISO    4     // PA6, pin 7
#define SPI_MOSI    5     // PA5, pin 8
#define SPI_SCK     6     // PA4, pin 9

#elif defined(__AVR_ATmega32U4__) //Arduino Leonardo 

#define RFM_IRQ     0	    // PD0, INT0, Digital3 
#define SS_DDR      DDRB
#define SS_PORT     PORTB
#define SS_BIT      6	    // Dig10, PB6

#define SPI_SS      17    // PB0, pin 8, Digital17
#define SPI_MISO    14    // PB3, pin 11, Digital14
#define SPI_MOSI    16    // PB2, pin 10, Digital16
#define SPI_SCK     15    // PB1, pin 9, Digital15

#else

// ATmega168, ATmega328, etc.
#define RFM_IRQ     2
#define SS_DDR      DDRB
#define SS_PORT     PORTB
#define SS_BIT      2     // for PORTB: 2 = d.10, 1 = d.9, 0 = d.8

#define SPI_SS      10    // PB2, pin 16
#define SPI_MOSI    11    // PB3, pin 17
#define SPI_MISO    12    // PB4, pin 18
#define SPI_SCK     13    // PB5, pin 19

#endif 

// RF12 command codes
#define RF_TXREG_WRITE  0xB800
#define RF_WAKEUP_TIMER 0xE000
#define RF_RECV_CONTROL 0x94A0

// RF12 status bits
#define RF_FIFO_BIT     0x8000
#define RF_POR_BIT      0x4000
#define RF_OVF_BIT      0x2000
#define RF_WDG_BIT      0x1000
#define RF_LBD_BIT      0x0400
#define RF_RSSI_BIT     0x0100

// bits in the node id configuration byte
#define NODE_BAND       0xC0        // frequency band
#define NODE_ACKANY     0x20        // ack on broadcast packets if set
#define NODE_ID         0x1F        // id of this node, as A..Z or 1..31

// transceiver states, these determine what to do with each interrupt
enum {
    TXCRC1, TXCRC2, TXTAIL, TXDONE, TXIDLE,
    UNINITIALIZED, POR_RECEIVED,	// indicates uninitialized RFM12b and Power-On-Reset
    TXRECV,
    TXPRE1, TXPRE2, TXPRE3, TXSYN1, TXSYN2,
};

static uint8_t cs_pin = SS_BIT;     // chip select pin

static uint8_t nodeid;              // address of this node
static uint8_t group;               // network group
static uint8_t band;				// network band
static volatile uint8_t rxfill;     // number of data bytes in rf12_buf
static volatile int8_t rxstate;     // current transceiver state
volatile uint16_t rfmstate;         // current power management setting of the RFM12 module
volatile uint16_t state;            // last seen rfm12b state
volatile uint8_t rf12_gotwakeup;	// 1 if there was a wakeup-call from RFM12
uint8_t drssi;                      // digital rssi state (see binary search tree below and rf12_getRSSI()
uint8_t drssi_bytes_per_decision;   // number of bytes required per drssi decision

const uint8_t drssi_dec_tree[] = {
  /* state,drssi,final, returned, up,      dwn */
	/*  A,   0,    no,    0001 */  3 << 4 | 4,
	/*  *,   1,    no,     --  */  0 << 4 | 2, // starting value
	/*  B,   2,    no,    0101 */  5 << 4 | 6
	/*  C,   3,   yes,    1000 */
	/*  D,   4,   yes,    1010 */
	/*  E,   5,   yes,    1100 */
	/*  F,   6,   yes,    1110 */
};  //                    \ Bit 1 indicates final state, others the signal strength


#define RETRIES     8               // stop retrying after 8 times
#define RETRY_MS    1000            // resend packet every second until ack'ed

static uint8_t ezInterval;          // number of seconds between transmits
static uint8_t ezSendBuf[RF12_MAXDATA]; // data to send
static char ezSendLen;              // number of bytes to send
static uint8_t ezPending;           // remaining number of retries
static long ezNextSend[2];          // when was last retry [0] or data [1] sent
static uint8_t fixedLength;				// length of non-standard packages
volatile uint16_t rf12_crc;         // running crc value
volatile uint8_t rf12_buf[RF_MAX];  // recv/xmit buf, including hdr & crc bytes
long rf12_seq;                      // seq number of encrypted packet (or -1)

static uint32_t seqNum;             // encrypted send sequence number
static uint32_t cryptKey[4];        // encryption key to use
void (*crypter)(uint8_t);           // does en-/decryption (null if disabled)

// function to set chip select pin from within sketch
void rf12_set_cs(uint8_t pin) {
#if defined(__AVR_ATmega32U4__)     //Arduino Leonardo 
  if (pin==10) cs_pin=6; 	    // Dig10, PB6     
  if (pin==9)  cs_pin=5; 	    // Dig9,  PB5	
  if (pin==8)  cs_pin=4; 	    // Dig8,  PB4            
#elif defined(__AVR_ATmega168__) || defined(__AVR_ATmega328__) || defined (__AVR_ATmega328P__) // ATmega168, ATmega328
  if (pin==10) cs_pin = 2; 	    // Dig10, PB2
  if (pin==9) cs_pin = 1;  	    // Dig9,  PB1
  if (pin==8) cs_pin = 0;  	    // Dig8,  PB0
#endif
}

// interrupts need to be disabled in a few spots
// do so by disabling the source of the interrupt, not all interrupts

#ifndef EIMSK
#define EIMSK GIMSK    // ATtiny
#endif

static void blockInterrupts () {
#if PINCHG_IRQ
    #if RFM_IRQ < 8
        bitClear(EIMSK, PCIE2);
    #elif RFM_IRQ < 14
        bitClear(EIMSK, PCIE0);
    #else
        bitClear(EIMSK, PCIE1);
    #endif
#else
    bitClear(EIMSK, INT0);
#endif
}

static void allowInterrupts () {
#if PINCHG_IRQ
    #if RFM_IRQ < 8
        bitSet(EIMSK, PCIE2);
    #elif RFM_IRQ < 14
        bitSet(EIMSK, PCIE0);
    #else
        bitSet(EIMSK, PCIE1);
    #endif
#else
    bitSet(EIMSK, INT0);
#endif
}


void rf12_spiInit () {
    bitSet(SS_PORT, cs_pin);
    bitSet(SS_DDR, cs_pin);
    digitalWrite(SPI_SS, 1);
    pinMode(SPI_SS, OUTPUT);
    pinMode(SPI_MOSI, OUTPUT);
    pinMode(SPI_MISO, INPUT);
    pinMode(SPI_SCK, OUTPUT);
#ifdef SPCR    
    SPCR = _BV(SPE) | _BV(MSTR);
#if F_CPU > 10000000
    // use clk/2 (2x 1/4th) for sending (and clk/8 for recv, see rf12_xfer)
    SPSR |= _BV(SPI2X);
#endif
#else
    // ATtiny
    USICR = bit(USIWM0);
#endif    
    pinMode(RFM_IRQ, INPUT);
    digitalWrite(RFM_IRQ, 1); // pull-up
}


// do a single byte SPI transfer
// only used from inside rf12_xfer and rf12_xferState which prevent race 
// conditions, don't call without disabling interrupts first!
static uint8_t rf12_byte (uint8_t out) {
#ifdef SPDR
    SPDR = out;
    // this loop spins 4 usec with a 2 MHz SPI clock
    while (!(SPSR & _BV(SPIF)))
        ;
    return SPDR;
#else
    // ATtiny
    USIDR = out;
    byte v1 = bit(USIWM0) | bit(USITC);
    byte v2 = bit(USIWM0) | bit(USITC) | bit(USICLK);
#if F_CPU <= 5000000
    // only unroll if resulting clock stays under 2.5 MHz
    USICR = v1; USICR = v2;
    USICR = v1; USICR = v2;
    USICR = v1; USICR = v2;
    USICR = v1; USICR = v2;
    USICR = v1; USICR = v2;
    USICR = v1; USICR = v2;
    USICR = v1; USICR = v2;
    USICR = v1; USICR = v2;
#else
    for (uint8_t i = 0; i < 8; ++i) {
        USICR = v1;
        USICR = v2;
    }
#endif
    return USIDR;
#endif
}

/// @details
/// This call provides direct access to the RFM12B registers. If you're careful
/// to avoid configuring the wireless module in a way which stops the driver
/// from functioning, this can be used to adjust some settings.
/// See the RFM12B wireless module documentation. 
static uint16_t rf12_xfer (uint16_t cmd) {
    blockInterrupts();

    // writing can take place at full speed, even 8 MHz works
    bitClear(SS_PORT, cs_pin);
    uint16_t res  = rf12_byte(cmd >> 8) << 8;
    res |= rf12_byte(cmd);
    bitSet(SS_PORT, cs_pin);

    allowInterrupts();
    return res;
}


/// @details
/// Requests RFM12 state from RF module and reads back a waiting data byte if there is
/// any.
/// One of two commands accessing SPI, so interrupts disabled in here
/// @param *data Pointer to  byte where to write the received data to (if any)
static uint16_t rf12_xferState (uint8_t *data) {
    blockInterrupts();

    // writing can take place at full speed, even 8 MHz works
    bitClear(SS_PORT, cs_pin);
    uint16_t res = rf12_byte(0x00) << 8;
    res |= rf12_byte(0x00);
    
    if (res & RF_FIFO_BIT && rxstate == TXRECV) {
        // slow down to under 2.5 MHz
#if F_CPU > 10000000
        bitSet(SPCR, SPR0);
#endif
        *data = rf12_byte(0x00);
#if F_CPU > 10000000
        bitClear(SPCR, SPR0);
#endif
    }
    
    bitSet(SS_PORT, cs_pin);

    allowInterrupts();
    return res;
}


/// @details
/// This call provides direct access to the RFM12B registers. If you're careful
/// to avoid configuring the wireless module in a way which stops the driver
/// from functioning, this can be used to adjust frequencies, power levels,
/// RSSI threshold, etc. See the RFM12B wireless module documentation.
///
/// OBSOLETE! Use rf12_xfer instead.
///
/// This function does no longer return anything.
/// @param cmd RF12 command, topmost bits determines which register is affected.
uint16_t rf12_control(uint16_t cmd) {
    return rf12_xfer(cmd);
}


/// @details
/// Brings RFM12 in idle-mode.
static void rf12_idle() {
	rfmstate &= ~B11110000; // switch off synthesizer, transmitter, receiver and baseband
	rfmstate |=  B00001000; // make sure crystal is running
	rf12_xfer(rfmstate);
}


/// @details
/// Handles a RFM12 interrupt depending on rxstate and the status reported by the RF
/// module. 
static void rf12_interrupt() {
    uint8_t in;
    state = rf12_xferState(&in);
    
    // data received or byte needed for sending
    if (state & RF_FIFO_BIT) {
	
	    if (rxstate == TXRECV) {  // we are receiving

            if (rxfill == 0 && group != 0)
                rf12_buf[rxfill++] = group;

            rf12_buf[rxfill++] = in;
        	rf12_crc = _crc16_update(rf12_crc, in);

    	    // do drssi binary-tree search
	        if ( drssi < 3 && ((rxfill-2)%drssi_bytes_per_decision)==0 ) {// not yet final value
	        	// top nibble when going up, bottom one when going down
	        	drssi = bitRead(state,8)
	        			? (drssi_dec_tree[drssi] & B1111)
	        			: (drssi_dec_tree[drssi] >> 4);
	            if ( drssi < 3 ) {     // not yet final destination, set new threshold
                	rf12_xfer(RF_RECV_CONTROL | drssi*2+1);
            	}
           	}

			// check if we got all the bytes (or maximum packet length was reached)
			if (fixedLength) {
				if (rxfill >= fixedLength || rxfill >= RF_MAX) {
					rf12_idle();
				}
			} else if (rxfill >= rf12_len + 5 || rxfill >= RF_MAX) {
       	    	rf12_idle();
			}
    	} else {                  // we are sending
	        uint8_t out;

    	    if (rxstate < 0) {
	            uint8_t pos = 3 + rf12_len + rxstate++;
            	out = rf12_buf[pos];
        	    rf12_crc = _crc16_update(rf12_crc, out);
    	    } else
	            switch (rxstate++) {
                	case TXSYN1: out = 0x2D; break;
            	    case TXSYN2: out = group; rxstate = - (2 + rf12_len); break;
        	        case TXCRC1: out = rf12_crc; break;
    	            case TXCRC2: out = rf12_crc >> 8; break;
	                case TXDONE: rf12_idle(); // fall through
                	default:     out = 0xAA;
            	}

        	rf12_xfer(RF_TXREG_WRITE | out);
		}
    }
    
    // power-on reset
    if (state & RF_POR_BIT) {
    	rxstate = POR_RECEIVED;
    }
    
    // got wakeup call
    if (state & RF_WDG_BIT) {
    	rf12_setWatchdog(0);
    	rf12_gotwakeup = 1;
    }
    
    // fifo overflow or buffer underrun - abort reception/sending
    if (state & RF_OVF_BIT) {
    	rf12_idle();
	    rxstate = TXIDLE;
    }
}

#if PINCHG_IRQ
    #if RFM_IRQ < 8
        ISR(PCINT2_vect) {
            while (!bitRead(PIND, RFM_IRQ))
                rf12_interrupt();
        }
    #elif RFM_IRQ < 14
        ISR(PCINT0_vect) { 
            while (!bitRead(PINB, RFM_IRQ - 8))
                rf12_interrupt();
        }
    #else
        ISR(PCINT1_vect) {
            while (!bitRead(PINC, RFM_IRQ - 14))
                rf12_interrupt();
        }
    #endif
#endif

static void rf12_recvStart () {
    rxfill = rf12_len = 0;
    rf12_crc = ~0;
#if RF12_VERSION >= 2
    if (group != 0)
        rf12_crc = _crc16_update(~0, group);
#endif
    rxstate = TXRECV;    
    drssi = 1;              // set drssi to start value
    rf12_xfer(RF_RECV_CONTROL | drssi*2+1);
    rfmstate |= B11011000; // enable crystal, synthesizer, receiver and baseband
    rf12_xfer(rfmstate);
}

#include <RF12.h> 
#include <Ports.h> // needed to avoid a linker error :(

byte rf12_recvDone();

/// @details
/// The timing of this function is relatively coarse, because SPI transfers are
/// used to enable / disable the transmitter. This will add some jitter to the
/// signal, probably in the order of 10 µsec.
///
/// If the result is true, then a packet has been received and is available for
/// processing. The following global variables will be set:
///
/// * volatile byte rf12_hdr -
///     Contains the header byte of the received packet - with flag bits and
///     node ID of either the sender or the receiver.
/// * volatile byte rf12_len -
///     The number of data bytes in the packet. A value in the range 0 .. 66.
/// * volatile byte rf12_data -
///     A pointer to the received data.
/// * volatile byte rf12_crc -
///     CRC of the received packet, zero indicates correct reception. If != 0
///     then rf12_hdr, rf12_len, and rf12_data should not be relied upon.
///
/// To send an acknowledgement, call rf12_sendStart() - but only right after
/// rf12_recvDone() returns true. This is commonly done using these macros:
///
///     if(RF12_WANTS_ACK){
///        rf12_sendStart(RF12_ACK_REPLY,0,0);
///      }
/// @see http://jeelabs.org/2010/12/11/rf12-acknowledgements/
uint8_t rf12_recvDone () {
    if (rxstate == TXRECV) {
		if (fixedLength) {
			if (rxfill >= fixedLength || rxfill >= RF_MAX) {
				rxstate = TXIDLE;
				rf12_crc = 1; //it is not a standard packet
				return 1;
			}
		} else if (rxfill >= rf12_len + 5 || rxfill >= RF_MAX) {
        rxstate = TXIDLE;
        if (rf12_len > RF12_MAXDATA)
            rf12_crc = 1; // force bad crc if packet length is invalid
        if (!(rf12_hdr & RF12_HDR_DST) || (nodeid & NODE_ID) == 31 ||
                (rf12_hdr & RF12_HDR_MASK) == (nodeid & NODE_ID)) {
            if (rf12_crc == 0 && crypter != 0)
                crypter(0);
            else
                rf12_seq = -1;
            return 1; // it's a broadcast packet or it's addressed to this node
			}
		}
    }
    if (rxstate == TXIDLE)
        rf12_recvStart();
    return 0;
}


// return signal strength calculated out of DRSSI bit
uint8_t rf12_getRSSI() {
	return (drssi<3 ? drssi*2+2 : 8|(drssi-3)*2);
}

void rf12_setBitrate(uint8_t rate) {
	const long int decisions_per_sec = 900;
	rf12_xfer(0xc600 | rate);
	unsigned long bits_per_second = (10000000UL / 29UL / (1 + (rate & 0x7f)) / (1 + (rate >> 7) * 7));
	unsigned long bytes_per_second = bits_per_second / 8;
	drssi_bytes_per_decision = (bytes_per_second + decisions_per_sec - 1) / decisions_per_sec;
}

void rf12_setFixedLength(uint8_t packet_len) {
	fixedLength = packet_len;
	if (packet_len > 0) fixedLength++;  //add a byte for the groupid
}

/// @details
/// Call this when you have some data to send. If it returns true, then you can
/// use rf12_sendStart() to start the transmission. Else you need to wait and
/// retry this call at a later moment.
///
/// Don't call this function if you have nothing to send, because rf12_canSend()
/// will stop reception when it returns true. IOW, calling the function
/// indicates your intention to send something, and once it returns true, you
/// should follow through and call rf12_sendStart() to actually initiate a send.
/// See [this weblog post](http://jeelabs.org/2010/05/20/a-subtle-rf12-detail/).
///
/// Note that even if you only want to send out packets, you still have to call
/// rf12_recvDone() periodically, because it keeps the RFM12B logic going. If
/// you don't, rf12_canSend() will never return true.
uint8_t rf12_canSend () {
    // if (rxstate == TXRECV && rxfill == 0 && rf12_getRSSI() < 2) {
    // TODO listen-before-send disabled until we figure out how to do it right
    if (rxstate == TXRECV && rxfill == 0) {
        rf12_idle();
        rxstate = TXIDLE;
        return 1;
    }
    return 0;
}

void rf12_sendStart (uint8_t hdr) {
    rf12_hdr = hdr & RF12_HDR_DST ? hdr :
                (hdr & ~RF12_HDR_MASK) + (nodeid & NODE_ID);
    if (crypter != 0)
        crypter(1);
    
    rf12_crc = ~0;
#if RF12_VERSION >= 2
    rf12_crc = _crc16_update(rf12_crc, group);
#endif
    rxstate = TXPRE1;
    rfmstate |= B00111000; // enable crystal, synthesizer and transmitter
    rf12_xfer(rfmstate);
    // no need to feed bytes, RFM module requests data using interrupts
}

/// @details
/// Switch to transmission mode and send a packet.
/// This can be either a request or a reply.
///
/// Notes
/// -----
///
/// The rf12_sendStart() function may only be called in two specific situations:
///
/// * right after rf12_recvDone() returns true - used for sending replies / 
///   acknowledgements
/// * right after rf12_canSend() returns true - used to send requests out
///
/// Because transmissions may only be started when there is no other reception
/// or transmission taking place.
///
/// The short form, i.e. "rf12_sendStart(hdr)" is for a special buffer-less
/// transmit mode, as described in this
/// [weblog post](http://jeelabs.org/2010/09/15/more-rf12-driver-notes/).
///
/// The call with 4 arguments, i.e. "rf12_sendStart(hdr, data, length, sync)" is
/// deprecated, as described in that same weblog post. The recommended idiom is
/// now to call it with 3 arguments, followed by a call to rf12_sendWait().
/// @param hdr The header contains information about the destination of the
///            packet to send, and flags such as whether this should be
///            acknowledged - or if it actually is an acknowledgement.
/// @param ptr Pointer to the data to send as packet.
/// @param len Number of data bytes to send. Must be in the range 0 .. 65.
void rf12_sendStart (uint8_t hdr, const void* ptr, uint8_t len) {
    rf12_len = len;
    memcpy((void*) rf12_data, ptr, len);
    rf12_sendStart(hdr);
}

/// @deprecated Use the 3-arg version, followed by a call to rf12_sendWait.
void rf12_sendStart (uint8_t hdr, const void* ptr, uint8_t len, uint8_t sync) {
    rf12_sendStart(hdr, ptr, len);
    rf12_sendWait(sync);
}

/// @details
/// Wait until transmission is possible, then start it as soon as possible.
/// @note This uses a (brief) busy loop and will discard any incoming packets.
/// @param hdr The header contains information about the destination of the
///            packet to send, and flags such as whether this should be
///            acknowledged - or if it actually is an acknowledgement.
/// @param ptr Pointer to the data to send as packet.
/// @param len Number of data bytes to send. Must be in the range 0 .. 65.
void rf12_sendNow (uint8_t hdr, const void* ptr, uint8_t len) {
  while (!rf12_canSend())
    rf12_recvDone(); // keep the driver state machine going, ignore incoming
  rf12_sendStart(hdr, ptr, len);
}
  
/// @details
/// Wait for completion of the preceding rf12_sendStart() call, using the
/// specified low-power mode.
/// @note rf12_sendWait() should only be called right after rf12_sendStart().
/// @param mode Power-down mode during wait: 0 = NORMAL, 1 = IDLE, 2 = STANDBY,
///             3 = PWR_DOWN. Values 2 and 3 can cause the millisecond time to 
///             lose a few interrupts. Value 3 can only be used if the ATmega 
///             fuses have been set for fast startup, i.e. 258 CK - the default
///             Arduino fuse settings are not suitable for full power down.
void rf12_sendWait (uint8_t mode) {
    // wait for packet to actually finish sending
    // go into low power mode, as interrupts are going to come in very soon
    while (rxstate != TXIDLE)
        if (mode) {
            // power down mode is only possible if the fuses are set to start
            // up in 258 clock cycles, i.e. approx 4 us - else must use standby!
            // modes 2 and higher may lose a few clock timer ticks
            set_sleep_mode(mode == 3 ? SLEEP_MODE_PWR_DOWN :
#ifdef SLEEP_MODE_STANDBY
                           mode == 2 ? SLEEP_MODE_STANDBY :
#endif
                                       SLEEP_MODE_IDLE);
            sleep_mode();
        }
}

/// @details
/// Attach interrupts for nodeid != 0
/// Detach interrupt for nodeid == 0
void rf12_interruptcontrol () {
#if PINCHG_IRQ
    #if RFM_IRQ < 8
        if ((nodeid & NODE_ID) != 0) {
            bitClear(DDRD, RFM_IRQ);      // input
            bitSet(PORTD, RFM_IRQ);       // pull-up
            bitSet(PCMSK2, RFM_IRQ);      // pin-change
            bitSet(PCICR, PCIE2);         // enable
        } else
            bitClear(PCMSK2, RFM_IRQ);
    #elif RFM_IRQ < 14
        if ((nodeid & NODE_ID) != 0) {
            bitClear(DDRB, RFM_IRQ - 8);  // input
            bitSet(PORTB, RFM_IRQ - 8);   // pull-up
            bitSet(PCMSK0, RFM_IRQ - 8);  // pin-change
            bitSet(PCICR, PCIE0);         // enable
        } else
            bitClear(PCMSK0, RFM_IRQ - 8);
    #else
        if ((nodeid & NODE_ID) != 0) {
            bitClear(DDRC, RFM_IRQ - 14); // input
            bitSet(PORTC, RFM_IRQ - 14);  // pull-up
            bitSet(PCMSK1, RFM_IRQ - 14); // pin-change
            bitSet(PCICR, PCIE1);         // enable
        } else
            bitClear(PCMSK1, RFM_IRQ - 14);
    #endif
#else
    if ((nodeid & NODE_ID) != 0)
        attachInterrupt(0, rf12_interrupt, LOW);
    else
        detachInterrupt(0);
#endif
}

/// @details
/// Call this once with the node ID (0-31), frequency band (0-3), and
/// optional group (0-255 for RFM12B, only 212 allowed for RFM12).
/// @param id The ID of this wireless node. ID's should be unique within the
///           netGroup in which this node is operating. The ID range is 0 to 31,
///           but only 1..30 are available for normal use. You can pass a single
///           capital letter as node ID, with 'A' .. 'Z' corresponding to the
///           node ID's 1..26, but this convention is now discouraged. ID 0 is
///           reserved for OOK use, node ID 31 is special because it will pick
///           up packets for any node (in the same netGroup).
/// @param band This determines in which frequency range the wireless module
///             will operate. The following pre-defined constants are available:
///             RF12_433MHZ, RF12_868MHZ, RF12_915MHZ. You should use the one
///             matching the module you have.
/// @param g Net groups are used to separate nodes: only nodes in the same net
///          group can communicate with each other. Valid values are 1 to 212. 
///          This parameter is optional, it defaults to 212 (0xD4) when omitted.
///          This is the only allowed value for RFM12 modules, only RFM12B
///          modules support other group values.
/// @returns the nodeId, to be compatible with rf12_config().
///
/// Programming Tips
/// ----------------
/// Note that rf12_initialize() does not use the EEprom netId and netGroup
/// settings, nor does it change the EEPROM settings. To use the netId and
/// netGroup settings saved in EEPROM use rf12_config() instead of
/// rf12_initialize. The choice whether to use rf12_initialize() or
/// rf12_config() at the top of every sketch is one of personal preference.
/// To set EEPROM settings for use with rf12_config() use the RF12demo sketch.
uint8_t rf12_initialize (uint8_t id, uint8_t b, uint8_t g) {
    nodeid = id;
    group = g;
    band = b;
    
    rf12_spiInit();
    rf12_interruptcontrol();
    // reset RFM12b module
    rf12_xfer(0xCA82); // enable software reset
    rf12_xfer(0xFE00); // do software reset
    rxstate = UNINITIALIZED;

    // wait until RFM12B is out of power-up reset, this could takes several *seconds*
    // normally about 50ms
    set_sleep_mode(SLEEP_MODE_IDLE);
    while (rxstate==UNINITIALIZED) {
#if PINCHG_IRQ
        while (digitalRead(RFM_IRQ)==LOW)
            rf12_interrupt();
#else
        sleep_mode();
#endif
    }
    rf12_restore(id, b, g);
    return nodeid;
}

/// @details
/// Call this when the settings of the RFM12B have been overwritten by the 
/// application with rf12_control(), to restore the original settings. Call
/// this with the node ID (0-31), frequency band (0-3), and
/// optional group (0-255 for RFM12B, only 212 allowed for RFM12).
/// @param id The ID of this wireless node. ID's should be unique within the
///           netGroup in which this node is operating. The ID range is 0 to 31,
///           but only 1..30 are available for normal use. You can pass a single
///           capital letter as node ID, with 'A' .. 'Z' corresponding to the
///           node ID's 1..26, but this convention is now discouraged. ID 0 is
///           reserved for OOK use, node ID 31 is special because it will pick
///           up packets for any node (in the same netGroup).
/// @param band This determines in which frequency range the wireless module
///             will operate. The following pre-defined constants are available:
///             RF12_433MHZ, RF12_868MHZ, RF12_915MHZ. You should use the one
///             matching the module you have.
/// @param g Net groups are used to separate nodes: only nodes in the same net
///          group can communicate with each other. Valid values are 1 to 212. 
///          This parameter is optional, it defaults to 212 (0xD4) when omitted.
///          This is the only allowed value for RFM12 modules, only RFM12B
///          modules support other group values.
void rf12_restore (uint8_t id, uint8_t b, uint8_t g) {
    nodeid = id;
    group = g;
    band = b;
    
    //interrupts may be attached or detached for OOK
    rf12_interruptcontrol();
    //undo settings for foreign-FSK use
    rf12_setFixedLength(0);
    blockInterrupts();
    rfmstate = 0x8205;              // RF_SLEEP_MODE
    rf12_xfer(rfmstate);            // DC (disable clk pin), enable lbd
        
    rf12_xfer(0x80C7 | (band << 4));// EL (ena TX), EF (ena RX FIFO), 12.0pF 
    rf12_xfer(0xA640);              // 868MHz 
    rf12_setBitrate(0x06);          // approx 49.2 Kbps, i.e. 10000/29/(1+6) Kbps
    rf12_xfer(0x94A2);              // VDI,FAST,134kHz,0dBm,-91dBm 
    rf12_xfer(0xC2AC);              // AL,!ml,DIG,DQD4 
    if (group != 0) {
        rf12_xfer(0xCA83);          // FIFO8,2-SYNC,!ff,DR 
        rf12_xfer(0xCE00 | group);  // SYNC=2DXX; 
    } else {
        rf12_xfer(0xCA8B);          // FIFO8,1-SYNC,!ff,DR 
        rf12_xfer(0xCE2D);          // SYNC=2D; 
    }
    rf12_xfer(0xC483);              // AFC@VDI,NO RSTRIC,!st,!fi,OE,EN 
    rf12_xfer(0x9850);              // !mp,90kHz,MAX OUT 
    rf12_xfer(0xCC77);              // OB1,OB0, LPX,!ddy,DDIT,BW0 
    rf12_xfer(0xE000);              // NOT USE 
    rf12_xfer(0xC800);              // NOT USE 
    rf12_xfer(0xC049);              // 1.66MHz,3.1V 

    rxstate = TXIDLE;
    
    allowInterrupts();
}

/// @details
/// This can be used to send out slow bit-by-bit On Off Keying signals to other
/// devices such as remotely controlled power switches operating in the 433,
/// 868, or 915 MHz bands.
///
/// To use this, you need to first call rf12initialize() with a zero node ID
/// and the proper frequency band. Then call rf12onOff() in the exact timing
/// you need for sending out the signal. Once done, either call rf12onOff(0) to
/// turn the transmitter off, or reinitialize the wireless module completely
/// with a call to rf12initialize().
/// @param value Turn the transmitter on (if true) or off (if false).
/// @note The timing of this function is relatively coarse, because SPI
/// transfers are used to enable / disable the transmitter. This will add some
/// jitter to the signal, probably in the order of 10 µsec.
void rf12_onOff (uint8_t value) {
  	if (value) {
  	    rfmstate |= B01111000; // switch on transmitter and all needed components
  	} else {
        rfmstate &= ~B00100000; // switch off transmitter
  	}
    rf12_xfer(rfmstate);
}

/// @details
/// This calls rf12_initialize() with settings obtained from EEPROM address
/// 0x20 .. 0x3F. These settings can be filled in by the RF12demo sketch in the
/// RFM12B library. If the checksum included in those bytes is not valid, 
/// rf12_initialize() will not be called.
///
/// As side effect, rf12_config() also writes the current configuration to the
/// serial port, ending with a newline.
/// @returns the node ID obtained from EEPROM, or 0 if there was none.
uint8_t rf12_config (uint8_t show) {
    uint16_t crc = ~0;
    for (uint8_t i = 0; i < RF12_EEPROM_SIZE; ++i)
        crc = _crc16_update(crc, eeprom_read_byte(RF12_EEPROM_ADDR + i));
    if (crc != 0)
        return 0;
        
    uint8_t nodeId = 0, group = 0;
    for (uint8_t i = 0; i < RF12_EEPROM_SIZE - 2; ++i) {
        uint8_t b = eeprom_read_byte(RF12_EEPROM_ADDR + i);
        if (i == 0)
            nodeId = b;
        else if (i == 1)
            group = b;
        else if (b == 0)
            break;
        else if (show)
            Serial.print((char) b);
    }
    if (show)
        Serial.println();
    
    rf12_initialize(nodeId, nodeId >> 6, group);
    return nodeId & RF12_HDR_MASK;
}

/// @details
/// This function can put the radio module to sleep and wake it up again.
/// In sleep mode, the radio will draw only one or two microamps of current.
///
/// @param n If RF12SLEEP (0), put the radio to sleep
///          If RF12WAKEUP (-1), wake the radio up so that the next call to 
///          rf12_recvDone() can restore normal reception.
void rf12_sleep (char n) {
    if (n < 0)
        rf12_idle();
    else {
        rfmstate &= ~B11111000; // make sure everything is switched off (except bod, wkup, clk)
        rf12_xfer(rfmstate);
    }
    rxstate = TXIDLE;
}


/// @details
/// Request an interrupt in the specified number of milliseconds. This Registers a wakeup-
/// interrupt in the RFM12 module. Use rf12_wakeup() to check if the wakeup-interrupt
/// fired. This allows very deep sleep states (timer off) while still being able to wakeup
/// because of the external interrupt. The RFM12b wakeup-timer only needs about 1.5µA.
/// Don't expect an accurate timing. It's about 10% off. Only one timer is supported.
/// @param m Number of milliseconds
void rf12_setWatchdog (unsigned long m) {
    // calculate parameters for RFM12 module
    // T_wakeup[ms] = m * 2^r
    char r=0;
    while (m > 255) {
        r  += 1;
        m >>= 1;
    }
    
    // Disable old wakeup-timer if enabled
    if (bitRead(rfmstate,1)) {  
        bitClear(rfmstate,1);
        rf12_xfer(rfmstate);
    }
    
    // enable wakeup call if we have to
    if (m>0) {
        // write time to wakeup-register
        rf12_xfer(RF_WAKEUP_TIMER | (r<<8) | m);
        // enable wakeup
        bitSet(rfmstate,1);
        rf12_xfer(rfmstate);
    }
}


/// @details
/// This checks the status of the RF12 low-battery detector. It wil be 1 when
/// the supply voltage drops below 3.1V, and 0 otherwise. This can be used to
/// detect an impending power failure, but there are no guarantees that the
/// power still remaining will be sufficient to send or receive further packets.
char rf12_lowbat () {
    return (state & RF_LBD_BIT) != 0;
}

/// @details
/// This function returns 1 if there was a wakeup-interrupt from the RFM12 module. Use it
/// together with rf12_setWatchdog() for ultra low-power watchdog.
char rf12_watchdogFired() {
	uint8_t res = rf12_gotwakeup;
	rf12_gotwakeup = 0;
	return res;
}

/// @details
/// Set up the easy transmission mechanism. The argument is the minimal number
/// of seconds between new data packets (from 1 to 255). With 0 as argument,
/// packets will be sent as fast as possible:
///
/// * On the 433 and 915 MHz frequency bands, this is fixed at 100 msec (10
///   packets/second).
/// 
/// * On the 866 MHz band, the frequency depends on the number of bytes sent:
///   for 1-byte packets, it will be up to 7 packets/second, for 66-byte bytes of
///   data it will be around 1 packet/second.
/// 
/// This function should be called after the RF12 driver has been initialized,
/// using either rf12_initialize() or rf12_config().
/// @param secs The minimal number of seconds between new data packets (from 1 
///             to 255). With a 0 argument, packets will be sent as fast as 
///             possible: on the 433 and 915 MHz frequency bands, this is fixed 
///             at 100 msec (10 packets/second). On 866 MHz, the frequency 
///             depends on the number of bytes sent: for 1-byte packets, it will 
///             be up to 7 packets/second, for 66-byte bytes of data it will be 
///             approx. 1 packet/second.
/// @note To be used in combination with rf12_easyPoll() and rf12_easySend().
void rf12_easyInit (uint8_t secs) {
    ezInterval = secs;
}

/// @details
/// This needs to be called often to keep the easy transmission mechanism going, 
/// i.e. once per millisecond or more in normal use. Failure to poll frequently 
/// enough is relatively harmless but may lead to lost acknowledgements.
/// @returns 1 = an ack has been received with actual data in it, use rf12len
///          and rf12data to access it. 0 = there is nothing to do, the last 
///          send has been ack'ed or more than 8 re-transmits have failed.
///          -1 = still sending or waiting for an ack to come in
/// @note To be used in combination with rf12_easyInit() and rf12_easySend().
char rf12_easyPoll () {
    if (rf12_recvDone() && rf12_crc == 0) {
        byte myAddr = nodeid & RF12_HDR_MASK;
        if (rf12_hdr == (RF12_HDR_CTL | RF12_HDR_DST | myAddr)) {
            ezPending = 0;
            ezNextSend[0] = 0; // flags succesful packet send
            if (rf12_len > 0)
                return 1;
        }
    }
    if (ezPending > 0) {
        // new data sends should not happen less than ezInterval seconds apart
        // ... whereas retries should not happen less than RETRY_MS apart
        byte newData = ezPending == RETRIES;
        long now = millis();
        if (now >= ezNextSend[newData] && rf12_canSend()) {
            ezNextSend[0] = now + RETRY_MS;
            // must send new data packets at least ezInterval seconds apart
            // ezInterval == 0 is a special case:
            //      for the 868 MHz band: enforce 1% max bandwidth constraint
            //      for other bands: use 100 msec, i.e. max 10 packets/second
            if (newData)
                ezNextSend[1] = now +
                    (ezInterval > 0 ? 1000L * ezInterval
                                    : (nodeid >> 6) == RF12_868MHZ ?
                                            13 * (ezSendLen + 10) : 100);
            rf12_sendStart(RF12_HDR_ACK, ezSendBuf, ezSendLen);
            --ezPending;
        }
    }
    return ezPending ? -1 : 0;
}

/// @details
/// Submit some data bytes to send using the easy transmission mechanism. The
/// data bytes will be copied to an internal buffer since the actual send may
/// take place later than specified, and may need to be re-transmitted in case
/// packets are lost of damaged in transit.
///
/// Packets will be sent no faster than the rate specified in the
/// rf12_easyInit() call, even if called more often.
///
/// Only packets which differ from the previous packet will actually be sent.
/// To force re-transmission even if the data hasn't changed, call 
/// "rf12_easySend(0,0)". This can be used to give a "sign of life" every once
/// in a while, and to recover when the receiving node has been rebooted and no
/// longer has the previous data.
///
/// The return value indicates whether a new packet transmission will be started
/// (1), or the data is the same as before and no send is needed (0).
///
/// Note that you also have to call rf12_easyPoll periodically, because it keeps
/// the RFM12B logic going. If you don't, rf12_easySend() will never send out
/// any packets.
/// @note To be used in combination with rf12_easyInit() and rf12_easyPoll().
char rf12_easySend (const void* data, uint8_t size) {
    if (data != 0 && size != 0) {
        if (ezNextSend[0] == 0 && size == ezSendLen &&
                                    memcmp(ezSendBuf, data, size) == 0)
            return 0;
        memcpy(ezSendBuf, data, size);
        ezSendLen = size;
    }
    ezPending = RETRIES;
    return 1;
}

// XXTEA by David Wheeler, adapted from http://en.wikipedia.org/wiki/XXTEA

#define DELTA 0x9E3779B9
#define MX (((z>>5^y<<2) + (y>>3^z<<4)) ^ ((sum^y) + \
                                            (cryptKey[(uint8_t)((p&3)^e)] ^ z)))

static void cryptFun (uint8_t send) {
    uint32_t y, z, sum, *v = (uint32_t*) rf12_data;
    uint8_t p, e, rounds = 6;
    
    if (send) {
        // pad with 1..4-byte sequence number
        *(uint32_t*)(rf12_data + rf12_len) = ++seqNum;
        uint8_t pad = 3 - (rf12_len & 3);
        rf12_len += pad;
        rf12_data[rf12_len] &= 0x3F;
        rf12_data[rf12_len] |= pad << 6;
        ++rf12_len;
        // actual encoding
        char n = rf12_len / 4;
        if (n > 1) {
            sum = 0;
            z = v[n-1];
            do {
                sum += DELTA;
                e = (sum >> 2) & 3;
                for (p=0; p<n-1; p++)
                    y = v[p+1], z = v[p] += MX;
                y = v[0];
                z = v[n-1] += MX;
            } while (--rounds);
        }
    } else if (rf12_crc == 0) {
        // actual decoding
        char n = rf12_len / 4;
        if (n > 1) {
            sum = rounds*DELTA;
            y = v[0];
            do {
                e = (sum >> 2) & 3;
                for (p=n-1; p>0; p--)
                    z = v[p-1], y = v[p] -= MX;
                z = v[n-1];
                y = v[0] -= MX;
            } while ((sum -= DELTA) != 0);
        }
        // strip sequence number from the end again
        if (n > 0) {
            uint8_t pad = rf12_data[--rf12_len] >> 6;
            rf12_seq = rf12_data[rf12_len] & 0x3F;
            while (pad-- > 0)
                rf12_seq = (rf12_seq << 8) | rf12_data[--rf12_len];
        }
    }
}

/// @details
/// This enables or disables encryption using the public domain XXTEA algorithm
/// by David Wheeler. The payload will be extended with 1 .. 4 bytes, containing
/// a 6..30-bit sequence number which is incremented in the sender for each new
/// packet.
///
/// The number of bits sent across depends on the number of padding bytes needed
/// to make the resulting payload an exact mulitple of 4 bytes. A longer
/// sequence number field can provide more protection against replay attacks
/// (note that verification of this sequence number must be implemented in the
/// receiver code).
///
/// Encrypted packets (and acknowledgements) must be 4..62 bytes long. Packets
/// less than 4 bytes will not be encrypted. On reception, the payload length is
/// adjusted back to the original length passed to rf12_sendStart().
///
/// There is a "long rf12seq" global which is set to the received sequence
/// number (only valid right after rf12recvDone() returns true). When encryption
/// is not enabled, this global is set to -1.
/// @param key Pointer to a 16-byte (128-bit) encryption key to use for all 
///            packet data. A null pointer disables encryption again. Note: 
///            this is an EEPROM address, not RAM! - RF12_EEPROM_EKEY is a great
///            value to use, as defined in the include file, but another address
///            can be specified if needed.
/// @see http://jeelabs.org/2010/02/23/secure-transmissions/
void rf12_encrypt (const uint8_t* key) {
    // by using a pointer to cryptFun, we only link it in when actually used
    if (key != 0) {
        for (uint8_t i = 0; i < sizeof cryptKey; ++i)
            ((uint8_t*) cryptKey)[i] = eeprom_read_byte(key + i);
        crypter = cryptFun;
    } else
        crypter = 0;
}
