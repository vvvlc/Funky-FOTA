#include <SoftPWM.h> //https://code.google.com/p/rogue-code/wiki/SoftPWMLibraryDocumentation

#define DELAY 40

uint8_t leds[8] = {13,8,2,1};

void setup()
{
  SoftPWMBegin();

  for (int i = 0; i < 8; i++)
    SoftPWMSet(leds[i], 0);

  SoftPWMSetFadeTime(ALL, 30, 200);

}

void loop()
{
  int i;

  for (i = 0; i < 3; i++)
  {
    SoftPWMSet(leds[i+1], 255);
    SoftPWMSet(leds[6-i], 255);
    SoftPWMSet(leds[i], 0);
    SoftPWMSet(leds[7-i], 0);
    delay(DELAY);
  }
  
  delay(250);
  
  for (i = 3; i > 0; i--)
  {
    SoftPWMSet(leds[i-1], 255);
    SoftPWMSet(leds[8-i], 255);
    SoftPWMSet(leds[i], 0);
    SoftPWMSet(leds[7-i], 0);
    delay(DELAY);
  }

  delay(250);
}
