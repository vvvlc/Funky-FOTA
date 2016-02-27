#!/bin/sh
rm *.hex*

PORT=ACM0

OD=$(pwd)
CATDIR=../../caterina-lilypadusb
cd $CATDIR

#compile catelina
make clean && make all && avr-objcopy -I ihex Caterina.hex -O binary Caterina.bin && avr-objdump -j .sec1 -d -m avr5 Caterina.hex  > Caterina.asm

SZ=$(wc -c <"Caterina.bin" )
if [ $SZ -gt 4096 ] ; then
	echo Caterina.bin is too big, remove: $(( $SZ - 4096)) bytes
	ls -la Caterina.bin
	exit
else
	echo We have space for: $((4096-$SZ)) bytes.
fi

cd $OD

if [ ! -e $CATDIR/Caterina.hex ] ; then
	exit
fi

BlinkGreen=BlinkGreenAndResetWatchDog
if [ ! -e $BlinkGreen.ino.elf ]; then
	echo Blink green is missing
	exit
fi

#size of each blink is 4446 = 0x115E
#if true; then  #Green after flash
avr-objcopy -O ihex -R .eeprom --adjust-vma=0x0000 Blink.ino.elf Blink.ino.hex00
avr-objcopy -O ihex -R .eeprom --adjust-vma=0x3802 $BlinkGreen.ino.elf $BlinkGreen.ino.hex38
avr-objcopy -O ihex -R .eeprom --adjust-vma=0x3802 Blink.ino.elf Blink.ino.hex38
avr-objcopy -O ihex -R .eeprom --adjust-vma=0x0000 $BlinkGreen.ino.elf $BlinkGreen.ino.hex00

#create binar version of file
avr-objcopy -O binary -R .eeprom --adjust-vma=0x0000 $BlinkGreen.ino.elf $BlinkGreen.ino.bin
avr-objcopy -O binary -R .eeprom --adjust-vma=0x0000 Blink.ino.elf Blink.ino.bin


if false; then  #orange after flash 
	#blink is in  0x00, green is in 0x38
	ln -s  Blink.ino.hex00 Blink.ino.hex
	ln -s  $BlinkGreen.ino.hex38 $BlinkGreen.ino.hex 
#na afdresu 0x3800-0x3802 ulozi hodnotu delku bin souboru v litle endean
	srec_cat -generate 0x3800 0x3802 -l-e-constant $(wc -c < "$BlinkGreen.ino.bin") 2 -o len.hex -intel
else
	#Blink green is 0x38 region blink is in 0x00
	ln -s  Blink.ino.hex38 Blink.ino.hex
	ln -s  $BlinkGreen.ino.hex00 $BlinkGreen.ino.hex 
#na afdresu 0x3800-0x3802 ulozi hodnotu delku bin souboru v litle endean
	srec_cat -generate 0x3800 0x3802 -l-e-constant $(wc -c < "Blink.ino.bin") 2 -o len.hex -intel
fi

#combine hexfiles and appends length of file
srec_cat Blink.ino.hex -intel  $BlinkGreen.ino.hex -intel len.hex -intel  $CATDIR/Caterina.hex -intel -o BlikBlinkGreenCatarina.hex -intel -Line_Length 44

#fill blank space via 0xFF
srec_cat BlikBlinkGreenCatarina.hex -intel -fill 0xFF 0x0 0x8000 -o BlikBlinkGreenCatarina.hex -intel -Line_Length 44

#convert it to binary
srec_cat BlikBlinkGreenCatarina.hex -intel -o BlikBlinkGreenCatarina.bin -binary


#exit
avrdude -v -v -patmega32u4 -cstk500v1 -b19200 -e -Ulock:w:0x3F:m -Uefuse:w:0xce:m -Uhfuse:w:0xd8:m -Ulfuse:w:0xff:m -P/dev/tty$PORT
avrdude -v -v -patmega32u4 -cstk500v1 -b19200 -Uflash:w:BlikBlinkGreenCatarina.hex:i -Ulock:w:0x2F:m   -P/dev/tty$PORT  || exit
#avrdude -v -v -patmega32u4 -cstk500v1 -b19200    -Ulock:w:0x3F:m -Uflash:w:Caterina.hex:i   -P/dev/tty$PORT

 
exit
avrdude -v -v -patmega32u4 -cstk500v1 -b19200 -P/dev/tty$PORT -U flash:r:post_flash.hex20:i
srec_cat post_flash.hex20 -intel -o post_flash.hex -intel -Line_Length 44

#exit

sleep 9

./read.sh
