// Electronic Bike Bell by Andrew Bedno AndrewBedno.com
// v1.2 2022.03.21
// Compile with "Huge APP" partition scheme to give at least 3MB to program.

// Configure physical details for your board type.
const int LED_pin = 19;
const int Button_pin = 32;
const int DAC1_pin = 25;
const int DAC2_pin = 26;
const long BoardMHz = 80;

// User interface configurables.
const int UI_timernum = 1;
const long LED_blinkdur = 10;  // Duration of LED blink at start of sound (/100s).
const long SkipTimeFirst = 85;  // Button held time for first skip to next sound (/100s).
const long SkipTimeContinue = 70;  // Button held time for subsequent skips (/100s).
const long SkipHinted = SkipTimeContinue/2;  // Button held time for reduced vol in prep to skip (/100s).
const uint8_t SkipVolume = 9;  // Reduced volume while skipping (0-127).
const uint8_t MaxVolume = 127;  // Max possible volume, 0-127 scale due to 0-255 full wav range.
uint8_t MasterVolume = MaxVolume;  // Current master volume, 0-127 scale.
const float VolumeDivisor = 1;  // Master level divisor, can convert to line out level but loses too much quality. 1=0-Vmax(3.7V), 1.85=0-2V

// Include WAVs
#include "sounds\ring.h"
#include "sounds\caw.h"
#include "sounds\ahooga.h"
#include "sounds\boat.h"
#include "sounds\steam.h"
#include "sounds\honk.h"
#include "sounds\bong.h"
#include "sounds\laughs.h"
#include "sounds\cheers.h"
#include "sounds\godzilla.h"
#include "sounds\howl.h"
#include "sounds\james.h"
#include "sounds\phaser.h"
#include "sounds\photon.h"
#include "sounds\beep.h"
#include "sounds\charge.h"
#include "sounds\shofar.h"
#include "sounds\unicorn.h"
#include "sounds\whistle.h"
#include "sounds\smash.h"

const int SoundsCnt = 20;
int SoundNum = 0;

bool SoundPlaying = false;
bool PrevPlaying = false;
bool ButtonPressed = false;
bool PrevPressed = false;
int ConsecutiveSkip = 0;

// Wav playback functions.

// Interrupt service routine frequency (50KHz)
#define DAC_BytesPerSec 50000
const int DAC_timernum = 0;
const uint8_t DAC_MidVal = 0x7f;
uint8_t DAC_LastVal = DAC_MidVal;
uint8_t DAC_CurrVal = DAC_MidVal;
uint32_t DAC_Idx = 0; // ISR ticks counter
float DAC_SampleRatio = 0.0;  // ISR ticks per wav byte.
bool Wav_Playing = false;

hw_timer_t * Wav_timer = nullptr;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// Adjust each wav byte to master volume with midpoint
uint8_t Wav_VolAdj(uint8_t ByteValue,uint8_t VolumeIn) {
	uint8_t AdjVal;
  VolumeIn = VolumeIn / VolumeDivisor;
	if(VolumeIn>DAC_MidVal) VolumeIn=DAC_MidVal;
	if(ByteValue==DAC_MidVal) return ByteValue;
	if(ByteValue>DAC_MidVal) {
		// + part of wave above 0, range 1 to 128, output 128 to 255
		AdjVal=ByteValue-DAC_MidVal;
		AdjVal=AdjVal*(float(VolumeIn)/float(DAC_MidVal));  // adjust ratio
		return AdjVal+DAC_MidVal;  // range adjust back to mid
	}
	// - part of wav, range 127 to 0, flip the value, ratio adjust, flip back for return.
	AdjVal=DAC_MidVal-ByteValue;
	AdjVal=AdjVal*(float(VolumeIn)/float(DAC_MidVal));
	return DAC_MidVal-AdjVal;
}

// Parse wav details.
uint16_t Wav_Rate = 0;
uint32_t Wav_Size = 0;
uint32_t Wav_Ofs = 0;
uint32_t Wav_Idx = 0;
const unsigned char *Wav_Data=nullptr;

// Convert 4 byte little-endian to a long.
#define longword(bfr, ofs) (bfr[ofs+3] << 24 | bfr[ofs+2] << 16 |bfr[ofs+1] << 8 |bfr[ofs+0])

void DAC_Write(uint8_t DACval) {
  DAC_LastVal = DACval;  // set output to mid point
  dacWrite(DAC1_pin,DACval);  dacWrite(DAC2_pin,DACval);
}

void Wav_Stop () {
  Wav_Playing = false;
  DAC_Idx = 0;
  DAC_Write(DAC_MidVal);
}

void Wav_Play (const unsigned char *Wav_In) {
  unsigned long ofs, siz;
  // Process header chunks.
  ofs = 12;
  siz = longword(Wav_In, 4);
  Wav_Rate = Wav_Ofs = 0;
  while (ofs < siz) {
    if (longword(Wav_In, ofs) == 0x61746164) {
      Wav_Size = longword(Wav_In, ofs+4);
      Wav_Idx = Wav_Ofs = ofs +8;
    }
    if (longword(Wav_In, ofs) == 0x20746d66) {
      Wav_Rate = longword(Wav_In, ofs+12);
    }
    ofs += longword(Wav_In, ofs+4) + 8;
  }
  DAC_SampleRatio = float(Wav_Rate)/float(DAC_BytesPerSec);
  Wav_Data = Wav_In;
  Wav_Stop();
  Wav_Playing = true;
}

// Main wav interrupt routine called DAC_BytesPerSec freq
void IRAM_ATTR Wav_ISR () {
  if (Wav_Playing) {
    Wav_Idx = long(float(DAC_Idx) * DAC_SampleRatio);
    if (Wav_Idx > Wav_Size) {
      Wav_Playing = false;
      DAC_Write(DAC_MidVal);
    } else {
      DAC_CurrVal = Wav_VolAdj(Wav_Data[Wav_Ofs+Wav_Idx],MasterVolume);
      if (DAC_CurrVal != DAC_LastVal) {  // Skip redundant setting.
        // Update both DACs.
        DAC_Write(DAC_CurrVal);
      }
      DAC_Idx++;
	  }
  }
}

void Wav_Init() {
	Wav_Stop();
  // Setup interrupt service routine for audio
	// Prescaler 80 for 1MHz freq, called every 20 counts for 50K/second (=DAC_BytesPerSec)
	Wav_timer = timerBegin(DAC_timernum, BoardMHz, true);
	timerAttachInterrupt(Wav_timer, &Wav_ISR, true);
	timerAlarmWrite(Wav_timer, 20, true);
	timerAlarmEnable(Wav_timer);
	delay(1);
}

// User Interface service handles timed things 100 times a sec.
hw_timer_t * UItimer = NULL;
long LED_centiSecs = -1;
long Skip_centiSecs = -1;
bool Debounced_out = false;  bool Debounce_prev = false;  bool Debounce_prevprev = false;  bool Debounce_temp = false;
bool SkipTrack = false;
void IRAM_ATTR UI_ISR() {
  if (LED_centiSecs>0) {
    LED_centiSecs--;
    if (LED_centiSecs<1) digitalWrite(LED_pin, LOW);
  }
  // Handle skip function.
  if (Skip_centiSecs>0) {
    Skip_centiSecs--;
    if (Skip_centiSecs<1) {
      if (ConsecutiveSkip < (SoundsCnt * 2)) {  // Detects stuck button.
        if (ButtonPressed) SkipTrack = true;
      }
    }
    if (Skip_centiSecs==(SkipTimeFirst-SkipHinted)) {
      MasterVolume = SkipVolume;  // Decrease volume in advance of skipping.
    }    
  } 
  // Debounce button by requiring three consecutive same states a tick apart.
  Debounce_prevprev = Debounce_prev;  Debounce_prev = Debounce_temp;
  Debounce_temp = (digitalRead(Button_pin)<1);
  if ( (Debounce_temp == Debounce_prev) && (Debounce_temp == Debounce_prevprev) ) { Debounced_out = Debounce_temp; }
}

void setup() {
  pinMode(LED_pin, OUTPUT);  // LED
  pinMode(32, INPUT_PULLUP);  // Button
  // UI timer service
  UItimer = timerBegin(UI_timernum, BoardMHz, true);
  timerAttachInterrupt(UItimer, &UI_ISR, true);
  timerAlarmWrite(UItimer, 10000, true); // 100th of a second (1,000,000/10,000)
  timerAlarmEnable(UItimer);
  delay(1);
  Wav_Init();
  digitalWrite(LED_pin, HIGH);  LED_centiSecs = LED_blinkdur;  // Flick LED
  Wav_Play(wav_ring);
}

void loop() {
  // Fetch and store button state.
  PrevPressed = ButtonPressed;
  ButtonPressed = Debounced_out;
  // Restore full volume and reset skipping whenever button is up.
  if ( (PrevPressed) && (! ButtonPressed) ) {
    ConsecutiveSkip = 0;  Skip_centiSecs = -1;  MasterVolume = MaxVolume;
  }
  
  // Fetch playing state of current sound allowing some asynchrony.
  PrevPlaying = SoundPlaying;
  SoundPlaying = Wav_Playing;

  // Handle skip flag from UI servicer.
  if (SkipTrack) {
    SkipTrack = false;
    if (SoundPlaying) Wav_Stop();
    // Reset everything to let next loop handle it.
    SoundPlaying = false;  PrevPlaying = false;
    ConsecutiveSkip++;
    MasterVolume = SkipVolume;  // Decrease volume while skipping.
    SoundNum++;
    if (SoundNum>=SoundsCnt) {
      SoundNum = 0;
      Skip_centiSecs = 2*SkipTimeContinue;  // Pause skipping longer on first sound to indicate looped.
    } else {
      Skip_centiSecs = SkipTimeContinue;  // Continue skipping
    }
    digitalWrite(LED_pin, HIGH);  LED_centiSecs = LED_blinkdur;  // Flick LED.
  }

  // Restart sound if button re-pressed and not skipping.
  if (Skip_centiSecs<1) {
    if (SoundPlaying) {
      if ( (ButtonPressed) && (! PrevPressed) ) {
        Wav_Stop();
        SoundPlaying = false;  PrevPlaying = false;
      }
    }
  }
 
  // If not playing, check for play button.
  if (! SoundPlaying) {
    if (ButtonPressed) {
      if (ConsecutiveSkip < (SoundsCnt * 2)) {  // Detects stuck button.
        // Start sound.
        switch(SoundNum) {
          case 1: Wav_Play(wav_caw); break;
          case 2: Wav_Play(wav_ahooga); break;
          case 3: Wav_Play(wav_boat); break;
          case 4: Wav_Play(wav_steam); break;
          case 5: Wav_Play(wav_honk); break;
          case 6: Wav_Play(wav_bong); break;
          case 7: Wav_Play(wav_laughs); break;
          case 8: Wav_Play(wav_cheers); break;
          case 9: Wav_Play(wav_godzilla); break;
          case 10: Wav_Play(wav_howl); break;
          case 11: Wav_Play(wav_james); break;
          case 12: Wav_Play(wav_phaser); break;
          case 13: Wav_Play(wav_photon); break;
          case 14: Wav_Play(wav_beep); break;
          case 15: Wav_Play(wav_charge); break;
          case 16: Wav_Play(wav_shofar); break;
          case 17: Wav_Play(wav_unicorn); break;
          case 18: Wav_Play(wav_whistle); break;
          case 19: Wav_Play(wav_smash); break;
          default: Wav_Play(wav_ring);
        }
        // Blink and check for skipping if not already skipping
        if (Skip_centiSecs<1) {
          Skip_centiSecs = SkipTimeFirst;
          digitalWrite(LED_pin, HIGH);  LED_centiSecs = LED_blinkdur;  // Flick LED
        }
      }
    }  
  }
}
