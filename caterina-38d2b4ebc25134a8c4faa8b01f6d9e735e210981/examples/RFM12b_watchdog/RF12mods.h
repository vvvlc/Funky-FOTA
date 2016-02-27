// 2009-02-09 <jc@wippler.nl> http://opensource.org/licenses/mit-license.php

#ifndef RF12_h
#define RF12_h

/// @file
/// RFM12B driver definitions

#include <stdint.h>

/// RFM12B Protocol version.
/// Version 1 did not include the group code in the crc.
/// Version 2 does include the group code in the crc.
#define RF12_VERSION    2

/// Shorthand for RFM12B group byte in rf12_buf.
#define rf12_grp        rf12_buf[0]
/// Shorthand for RFM12B header byte in rf12_buf.
#define rf12_hdr        rf12_buf[1]
/// Shorthand for RFM12B length byte in rf12_buf.
#define rf12_len        rf12_buf[2]
/// Shorthand for first RFM12B data byte in rf12_buf.
#define rf12_data       (rf12_buf + 3)

/// RFM12B CTL bit mask.
#define RF12_HDR_CTL    0x80
/// RFM12B DST bit mask.
#define RF12_HDR_DST    0x40
/// RFM12B ACK bit mask.
#define RF12_HDR_ACK    0x20
/// RFM12B HDR bit mask.
#define RF12_HDR_MASK   0x1F

/// RFM12B Maximum message size in bytes.
#define RF12_MAXDATA    66

#define RF12_433MHZ     1   ///< RFM12B 433 MHz frequency band.
#define RF12_868MHZ     2   ///< RFM12B 868 MHz frequency band.
#define RF12_915MHZ     3   ///< RFM12B 915 MHz frequency band.

// EEPROM address range used by the rf12_config() code
#define RF12_EEPROM_ADDR ((uint8_t*) 0x20)  ///< Starting offset.
#define RF12_EEPROM_SIZE 32                 ///< Number of bytes.
#define RF12_EEPROM_EKEY (RF12_EEPROM_ADDR + RF12_EEPROM_SIZE) ///< EE start.
#define RF12_EEPROM_ELEN 16                 ///< EE number of bytes.

/// Shorthand to simplify detecting a request for an ACK.
#define RF12_WANTS_ACK ((rf12_hdr & RF12_HDR_ACK) && !(rf12_hdr & RF12_HDR_CTL))
/// Shorthand to simplify sending out the proper ACK reply.
#define RF12_ACK_REPLY (rf12_hdr & RF12_HDR_DST ? RF12_HDR_CTL : \
            RF12_HDR_CTL | RF12_HDR_DST | (rf12_hdr & RF12_HDR_MASK))
            
// options for RF12_sleep()
#define RF12_SLEEP 0        ///< Enter sleep mode.
#define RF12_WAKEUP -1      ///< Wake up from sleep mode.

/// Running crc value, should be zero at end.
extern volatile uint16_t rf12_crc;
/// Recv/xmit buf including hdr & crc bytes.
extern volatile uint8_t rf12_buf[];
/// Seq number of encrypted packet (or -1).
extern long rf12_seq;

/// Option to set RFM12 CS (or SS) pin for use on different hardware setups.
/// Set to Dig10 by default for JeeNode. Can be Dig10, Dig9 or Dig8
void rf12_set_cs(uint8_t pin);

/// Only needed if you want to init the SPI bus before rf12_initialize() does.
void rf12_spiInit(void);

/// Call this once with the node ID, frequency band, and optional group.
uint8_t rf12_initialize(uint8_t id, uint8_t band, uint8_t group=0xD4);

/// Call this to restore the initial settings when switching from OOK to FSK
/// with the node ID, frequency band, and optional group.
/// The RFM12B will not be reset, saving a minumum 0f 50ms.
void rf12_restore(uint8_t id, uint8_t band, uint8_t group=0xD4);

/// Initialize the RFM12B module from settings stored in EEPROM by "RF12demo"
/// don't call rf12_initialize() if you init the hardware with rf12_config().
/// @return the node ID as 1..31 value (1..26 correspond to nodes 'A'..'Z').
uint8_t rf12_config(uint8_t show =1);

/// Call this frequently, returns true if a packet has been received.
uint8_t rf12_recvDone(void);

/// Returns RSSI approximation for last received packet
uint8_t rf12_getRSSI();

/// Use this function to change the data rate after rf12_initialize. 
void rf12_setBitrate(uint8_t rate);

/// Use this function to switch the driver into fixed packet length mode
/// packet_len <  RF12_MAXDATA, 0 = disable, receive standard format packages.
void rf12_setFixedLength(uint8_t packet_len);

/// Call this to check whether a new transmission can be started.
/// @return true when a new transmission may be started with rf12_sendStart().
uint8_t rf12_canSend(void);

/// Call this only when rf12_recvDone() or rf12_canSend() return true.
void rf12_sendStart(uint8_t hdr);
/// Call this only when rf12_recvDone() or rf12_canSend() return true.
void rf12_sendStart(uint8_t hdr, const void* ptr, uint8_t len);
/// Deprecated: use rf12_sendStart(hdr,ptr,len) followed by rf12_sendWait(sync).
void rf12_sendStart(uint8_t hdr, const void* ptr, uint8_t len, uint8_t sync);
/// This variant loops on rf12_canSend() and then calls rf12_sendStart() asap.
void rf12_sendNow(uint8_t hdr, const void* ptr, uint8_t len);

/// Wait for send to finish.
/// @param mode sleep mode 0=none, 1=idle, 2=standby, 3=powerdown.
void rf12_sendWait(uint8_t mode);

/// This simulates OOK by turning the transmitter on and off via SPI commands.
/// Use this only when the radio was initialized with a fake zero node ID.
void rf12_onOff(uint8_t value);

/// Power off the RFM12B if n==0
/// @note if off, calling this with -1 can be used to bring the RFM12B back up.
void rf12_sleep(char n);

/// Request a wakeup-event after this many ms.
/// Maximum time is about 2 years but limited by unsigned long which gives about 49 days
/// @note set to 0 to disable a running wakeup-timer
void rf12_setWatchdog(unsigned long ms);

/// Checks if there was a wakeup-call from the RFM12 watchdog.
/// @return true if RFM12 fired a watchdog interrupt
char rf12_watchdogFired();

/// Return true if the supply voltage is below 3.1V.
char rf12_lowbat(void);

/// Set up the easy tranmission mode, arg is number of seconds between packets.
void rf12_easyInit(uint8_t secs);

/// Call this often to keep the easy transmission mode going.
char rf12_easyPoll(void);

/// Send new data using easy transmission mode, buffer gets copied to driver.
char rf12_easySend(const void* data, uint8_t size);

/// Enable encryption (null arg disables it again).
void rf12_encrypt(const uint8_t*);

/// Low-level control of the RFM12B via direct register access.
/// http://tools.jeelabs.org/rfm12b is useful for calculating these.
uint16_t rf12_control(uint16_t cmd);

/// See http://blog.strobotics.com.au/2009/07/27/rfm12-tutorial-part-3a/
/// Transmissions are packetized, don't assume you can sustain these speeds! 
///
/// @note Data rates are approximate. For higher data rates you may need to
/// alter receiver radio bandwidth and transmitter modulator bandwidth.
/// Note that bit 7 is a prescaler - don't just interpolate rates between
/// RF12_DATA_RATE_3 and RF12_DATA_RATE_2.
enum rf12DataRates {
    RF12_DATA_RATE_CMD = 0xC600,
    RF12_DATA_RATE_9 = RF12_DATA_RATE_CMD | 0x02,  // Approx 115200 bps
    RF12_DATA_RATE_8 = RF12_DATA_RATE_CMD | 0x05,  // Approx  57600 bps
    RF12_DATA_RATE_7 = RF12_DATA_RATE_CMD | 0x06,  // Approx  49200 bps
    RF12_DATA_RATE_6 = RF12_DATA_RATE_CMD | 0x08,  // Approx  38400 bps
    RF12_DATA_RATE_5 = RF12_DATA_RATE_CMD | 0x11,  // Approx  19200 bps
    RF12_DATA_RATE_4 = RF12_DATA_RATE_CMD | 0x23,  // Approx   9600 bps
    RF12_DATA_RATE_3 = RF12_DATA_RATE_CMD | 0x47,  // Approx   4800 bps
    RF12_DATA_RATE_2 = RF12_DATA_RATE_CMD | 0x91,  // Approx   2400 bps
    RF12_DATA_RATE_1 = RF12_DATA_RATE_CMD | 0x9E,  // Approx   1200 bps
    RF12_DATA_RATE_DEFAULT = RF12_DATA_RATE_7,
};

#endif
