#!/bin/sh
rm new_firmware.hex*
avrdude -v -v -patmega32u4 -cstk500v1 -b19200 -P/dev/ttyACM0 -U flash:r:new_firmware.hex20:i
srec_cat new_firmware.hex20  -intel -o new_firmware.hex -intel -Line_Length 44
