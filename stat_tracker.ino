//Includes
#include <SPI.h>
#include <EEPROM.h>
#include <U8glib.h>
#include <MFRC522.h>
#include <stdio.h>

//Defines for all the external devices
//Display
#define PIN_DISPLAY_DATA 3
#define PIN_SPI_DISPLAY_SS 12
#define PIN_DISPLAY_RESET 11
//NFC Reader
#define PIN_SPI_NFC_SS 10
#define PIN_NFC_RESET 9
//Buttons
#define PIN_BUTTON_TOGGLE 8
//AIN0 in analog comparator is digital 7, should be voltage from IR detector
#define PIN_BUTTON_PUSH1 6
#define PIN_BUTTON_PUSH2 5
//Status LEDs
#define PIN_LED_STATUS1 13 //Onboard LED
//Analogs
#define PIN_ANALOG_IR_CALIB_READ A0 //ANALOG 0 (pin 14) | Where to read the initial calib value
#define CALIB_DEC_VALUE 0 //The amount to decrese the calibration value in 5/1024V (should be smol)
#define PIN_ANALOG_IR_CALIB_WRITE 5 //DIGITAL 5 | Where to write as a PWM signal to get converted to DC
#define PIN_ANALOG_IR_COMPARE_MUX4_0 0x6 //ANALOG 1 (pin 15) is ADC6, mask 0x6 | Where to read the calib compare signal that gets converted is the ADC Mux Mask value

//EEPROM Addresses
#define EEPROM_ADDR_COUNT 0 //Address for the count variable

U8GLIB u8g;
extern const u8g_fntpgm_uint8_t displayDotsMod62[] U8G_FONT_SECTION("displayDotsMod62"); //Custom font
char displayStrs[5][7] = {"ERROR!","","USER","UNKNWN",""};
short displayState = 0;
char* displayStr = NULL;
unsigned long displayStateTimeOffset = 0;
unsigned long time = 0;

MFRC522* mfrc522 = NULL;
MFRC522::Uid userUid;
bool isUserKnown = false;
bool isUserPresent = false;
bool userCheckNeeded = false;
short userMissCount = 0;
unsigned long lastUserCheckTime = 0;

volatile unsigned long lastDartTime = 0;
volatile unsigned int count = 0;
volatile bool countDirty = false;
char countStr[7] = "000000";

unsigned int calib = 0; //Duty cycle of PWM for analog calibration of IR sensor

//Function prototype
void strpad(char*, int, char);
int uidEqual(MFRC522::Uid, MFRC522::Uid);

void setup()
{
  //Status LED Startup
  digitalWrite(PIN_LED_STATUS1, HIGH);

  //Init SPI Bus
  SPI.begin();

  //Get calibrate value
  delay(1000);
  unsigned int calibRead = analogRead(PIN_ANALOG_IR_CALIB_READ);
  //Get duty cycle of PWM output for this read voltage
  //c = (calibRead * (5/1024)) //Vout
  //c = c / 5 //Vout/5V = duty cycle as float
  //c = c * 255 //0-255 value for duty cycle as needed for analogWrite
  float dutyFloat = (float)(calibRead - CALIB_DEC_VALUE) * (1.0f/1024.0f);//Duty cycle as float
  calib = (unsigned int)(dutyFloat * 255);//Duty as 0-255 int
  analogWrite(PIN_ANALOG_IR_CALIB_WRITE, calib);

  //Setup the Display
  u8g = U8GLIB_NHD31OLED_2X_GR(PIN_SPI_DISPLAY_SS,PIN_DISPLAY_DATA,PIN_DISPLAY_RESET);
  u8g.setFont(displayDotsMod62);
  u8g.setColorIndex(1); // Instructs the display to draw with a pixel on.
  
  //Setup the NFC Reader
  mfrc522 = new MFRC522(PIN_SPI_NFC_SS,PIN_NFC_RESET);
  mfrc522->PCD_WriteRegister(MFRC522::RFCfgReg, (0x07<<4)); // Set Rx Gain to max
  mfrc522->PCD_Init();    // Init MFRC522
  
  //Buttons
  pinMode(PIN_BUTTON_TOGGLE, INPUT);
  pinMode(PIN_BUTTON_PUSH1, INPUT);
  pinMode(PIN_BUTTON_PUSH2, INPUT);
  pinMode(PIN_LED_STATUS1, OUTPUT);

  //Analog Settings - AVR Processor Specific
  //For the IR Collector, when the beam breaks and then rejoins, the rejoin triggers a
  //dip in voltage that they rises. We use this rise after the dip to detect a shot
  //NORMAL VOLTAGE: 4.88V
  //DIP VOLTAGE: <4.20V? (have to play with this)
  //AIN+ comes from AIN0 (D7 on micro)
  //AIN- comes from #define PIN_ANALOG_IR_COMPARE_MUX4_0, the mux settings with MSB (MUX5) as 0
  noInterrupts();

  //ADCSRA - ADC Status Reg A
  ADCSRA &= ~(0x1<<ADEN); //ADC Disabled (so that we can use for comparator)
  
  //ADCSRB - ADC Status Reg B
  ADCSRB |= (0x1<<ACME); //ADC Mux enable
  ADCSRB &= ~(0x1<<MUX5); //clr mux bit 5

  //ADMUX - ADC Mux
  ADMUX &= ~(0x1f<<MUX0);  //CLR Mux
  ADMUX |= PIN_ANALOG_IR_COMPARE_MUX4_0<<MUX0; //Set to IR_COMPARE_MUX4_0

  //ACSR - Analog Comparator
  ACSR |= 0x1<<ACD; //Disable comparator
  
  ACSR |= (0x1<<ACI);  //Clear any interrupt flag
  ACSR |= (0x1<<ACIE); //Enable interrupt
  ACSR &= ~(0x3<<ACIS0);//CLR Mode
  ACSR |= (0x2<<ACIS0);//Use falling edge

  ACSR &= ~(0x1<<ACD); //Enable comparator

  //DIDR1 - Digital input disable register
  DIDR1 |= (1<<AIN0D); //Disable for our AIN0 pin to save power

  //Further below is an ISR for ANALOG_COMP_vect, the analog comparator
  interrupts();

  //Get last count
  //EEPROM.get(EEPROM_ADDR_COUNT, count);

  //Status LED Exist Startupa
  digitalWrite(PIN_LED_STATUS1, LOW);

  time = millis();
}

//ISR for analog comparator
ISR(ANALOG_COMP_vect)
{
  if(!isUserPresent)
    return;

  //Update count, alert that it changed
  if(millis() > lastDartTime + 100){ //Interrupt "debounce" by 100ms
    //Update count, alert that it changed
    count++;
    countDirty = true;
    lastDartTime = millis();
  }

  int what = digitalRead(PIN_LED_STATUS1);
  digitalWrite(PIN_LED_STATUS1, !what);
}

void loop()
{
  time = millis();
  int displayState = ((time - displayStateTimeOffset) / 1000) % 8; //0-7 integer incrementing every second

  //Needs a rewrite every time or it decreases
  analogWrite(PIN_ANALOG_IR_CALIB_WRITE, calib);
  
  //Update the Count String (if needed)
  if(countDirty)
  {
    itoa(count,countStr,10);
    strpad(countStr,6,'0'); //Pad to 6 characters
    //Save count
    //EEPROM.put(EEPROM_ADDR_COUNT, count);
    countDirty = false;
  }
  
  //Update the Display String
  if(isUserPresent)
  {
    //Just count
    displayStr = countStr;
  }
  else
  {
    //Error user unknown message
    if(displayState < 5) //Less than 5 display the messages
      displayStr = displayStrs[displayState];
    else //Otherwise use the count value
      displayStr = countStr;
  }
  
  //Update the Display with string
  u8g.firstPage();
  do {
    //Draw the count @ (0,60) with 
    u8g.drawStr(20, 60, displayStr);
  } while( u8g.nextPage() );
  
  //Check the NFC Reader
  //Checking for the first read to use as user
  if(!isUserKnown)
  {
    if(mfrc522->PICC_IsNewCardPresent() && mfrc522->PICC_Select(&(mfrc522->uid)))
    {
      isUserKnown = true;
      isUserPresent = true;
      userUid = mfrc522->uid;
      lastUserCheckTime = time;
    }
  }
  //Checking after user has left for the same user
  else if(!isUserPresent)
  {
    if(mfrc522->PICC_IsNewCardPresent() && mfrc522->PICC_Select(&(mfrc522->uid)))
    {
      if(uidEqual(mfrc522->uid, userUid))
      {
        isUserPresent = true;
        lastUserCheckTime = time;
      }
    }
  }
  //Checking to see if the user has left
  else if(userCheckNeeded)
  {
    //Check for the old Uid we got first
    //First, we need to RequestA to put PICCs in READY state
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);
    byte result = mfrc522->PICC_RequestA(bufferATQA, &bufferSize);
    if(result != MFRC522::STATUS_OK && result != MFRC522::STATUS_COLLISION)
    {
      userMissCount++;
    }
    else
    {
      //Select the PICC
      result = mfrc522->PICC_Select(&userUid);
      
      //Did a card with the Uid respond?
      if(result != MFRC522::STATUS_OK && result != MFRC522::STATUS_COLLISION)
      {
        //Nope, add a miss
        userMissCount++;
      }
      else
      {
        //Yup, reset the miss count
        userMissCount = 0;
      }
    }
    
    //Wait for two misses before "logging off" user
    if(userMissCount >= 2)
    {
      userMissCount = 0;
      isUserPresent = false;
      displayStateTimeOffset = time;
    }
    //New lastUserCheckTime
    userCheckNeeded = false;
    lastUserCheckTime = time;
  }
  //Checking to see if we should check to see if the user has left
  //Sets the userCheckNeeded flag every second
  else if(!userCheckNeeded)
  {
    //Only check for a miss ever 1sec
    if(time - lastUserCheckTime > 1000)
    {
      userCheckNeeded = true;
    }
  }
}

//Pads a string at strPtr with character what to fixed width padLen
//strPtr needs to have padLen amount allocated + 1 null char otherwise underfined behavior
void strpad(char* str, int padLen, char what)
{
  int strLen = strlen(str);
  if(padLen <= strLen)
    return;

  char tmpStr[padLen + 1];
  int padStrLen = padLen - strLen;
  for(int c=0; c < padLen; c++)
  {
    if(c < padStrLen)
      tmpStr[c] = what;
    else
      tmpStr[c] = str[c - padStrLen];
  }
  tmpStr[padLen] = '\0';
  strcpy(str, tmpStr);
}

//Checks if two MFRC522::Uid's equal each other by comparing their id bytes
int uidEqual(MFRC522::Uid obj1, MFRC522::Uid obj2)
{
  //Compare sizes
  if(obj1.size != obj2.size)
    return 0;

  //Compare content
  int idCnt = 0;
  while(idCnt < obj1.size)
  {
    if(obj1.uidByte[idCnt] != obj2.uidByte[idCnt])
      return 0;
    idCnt++;
  }

  return 1;
}
