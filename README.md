# FOTA/Wireless Programming for Funky V3

[WIKI](https://github.com/vvvlc/Funky-FOTA/wiki/FOTA-on-Funky) describes how to setup FOTA on Funky.

I wanted to have [FOTA](http://en.wikipedia.org/wiki/Over-the-air_programming)/[Wireless programming](http://lowpowerlab.com/blog/category/moteino/wireless-programming/) capability for my project. Unfortunately [RFM69 library](https://github.com/LowPowerLab/RFM69) does not fit 4KB [bootloader](http://www.hackersworkbench.com/intro-to-bootloaders-for-avr) area therefore I could not follow [standard self-programming mechanism](http://www.engineersgarage.com/embedded/avr-microcontroller-projects/How-to-Use-SPM-for-Flash-to-Flash-Programming) (store data comming from a serial stream to internal flash). I wanted to reuse [Felix's wireless programming library](https://github.com/LowPowerLab/WirelessProgramming) however [Funky V3](http://harizanov.com/wiki/wiki-home/funky-v3/) does not have external [spi flash](http://www.instructables.com/id/How-to-Design-with-Discrete-SPI-Flash-Memory/). Therefore I had to change layout of the internal 32KB flash. I was influenced by video: [Intermediate memory bootloaders](https://www.youtube.com/watch?v=jbLy6kE-Szg).

Standard layout for [ATMEGA32U4](http://www.atmel.com/devices/atmega32u4.aspx) with [Caterina bootloader](https://github.com/arduino/Arduino/blob/master/hardware/arduino/avr/bootloaders/caterina/Caterina.c) has two main sections. 
 * The application section (aps) 0x0000-0x6FFF (28KB) and 
 * bootloader section (bls) 0x7000-0x7FFF (4KB). 

I split aps into two equal areas. First is an app area 0x0000-0x37FF (14KB) and the rest 0x3800-0x7FFF (14KB) is something like external spi flash area, let's call it temp area. The first disadvantage is clear we have only 14KB instead of 28KB for our application code. To be honest it is even worst. 12KB out of 14KB is occupied by RFM69 driver and wireless programing code. The bottom line is that we have around 1KB for our app. It may seem to be ridiculously small but it is sufficient for my app that is supposed to count led flashes on my watt-meter and send them via RFM69 to [RFM69PI V3](https://wiki.openenergymonitor.org/index.php/RFM69Pi_V3) gateway for further processing. Later on I would like to measure other things such as temperature voltage of battery, … moreover I don't have easy access to Funky node therefore FOTA is needed.

## Changes in Caterina Bootloader
 - I modified [Caterina bootloader](https://github.com/mharizanov/new_Funky/tree/master/caterina-lilypadusb) to expose a function that wraps [SPM](http://www.atmel.com/webdoc/avrassembler/avrassembler.wb_SPM.html) instruction. I found inspiration in video
 [arduinos (and other avrs) write to own flash](http://hackaday.com/2015/07/03/arduinos-and-other-avrs-write-to-own-flash/). Btw there is an interesting trick how to invoke SPM
[without wrapping function](http://oneweekwonder.blogspot.cz/2014/07/bootjacker-amazing-avr-bootloader-hack.html), [src code](https://gist.github.com/Snial/2d516b6305165bf81415).
 - I added function that updates the app area with code stored in temp area. More or less what [DualOptiboot](https://github.com/LowPowerLab/DualOptiboot) does.

The only remaining piece is the update of temp area with firmware coming over the air.

## Changes in Wireless Programming Library 
Modified wireless programming library writes firmware coming from the air into temp area instead of external spi flash. I removed all debugging messages to shrink size of this library in favor of space for application code.

## Layout of Flash Memory 
| Start address | Stop address | Size        | Description                                                                      |
|---------------|--------------|-------------|----------------------------------------------------------------------------------|
| 0x0000        | 0x37FF       | 14KB=14336B | Application code (app area)                                                      |
| 0x3800        | 0x3801       | 2B          | Length of data in temp area  (Length >14334 means temp area contains invalid data) |
| 0x3802        | 0x6FFF       | 14334B      | Temp area                                                                        |
| 0x7000        | 0x7FFF       | 4KB=4096    | Bootloader area (bls)                                                            |

## Complete Flow of FOTA Process 

1. Let’s power on Funky (we assume that Temp area contains invalid data)
2. Bootloader is active
3. Is there a new code in Temp area (check value at address 0x3800)? 
No, therefore pass control to application, ie. jmp 0x0000
4. Application is active and processing requests or sending data via RFM69
5. Application checks if there is a request to update firmware. (if not go to 4)
6. Write firmware to Temp area (using externalized wrapper function for SPM instruction) and reboot Funky via watchdog.
7. Bootloader is active
8. Are there valid data in Temp area (value at 0x3800 contains length of data and is < 14334)
9. Copy content of Temp area to app area
10. Erase page at 0x3800, this set length to 0xFFFF, ie invalidates content of Temp area
11. Pass control to application, ie. jmp 0x0000

## Potential Improvements 

 * verification of checksum of firmware prior to update of app area.
 * more space in the app area, there are several techniques how to get there
  * Shrink RFM69 library and Wireless programming library
  * Compression that would shrink temp area in favour of app area. The app and temp area sizes have to be aligned to SPM_PAGESIZE (128B).
  * Squeeze RFM69 driver into 4KB. This could eliminate necessity of temp area because bootloader could flash data directly to app area. This seems to be best approach but the most complicated one.
 * introduce [faster mode](http://lowpowerlab.com/blog/2016/01/21/wireless-programming-just-got-50-faster/) that increases transfer rate
 * allow Arduino IDE to update firmware over the air

## Credits
 * Martin Harizanov for [Funky V3](http://harizanov.com/wiki/wiki-home/funky-v3/) 
 * LowPowerLab for [RFM69 and Wireless programming](http://lowpowerlab.com/)
