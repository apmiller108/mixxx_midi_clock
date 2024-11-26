/*
 * mixxx_midi_clock.ino
 *
 * Author: alex miller
 * https://github.com/apmiller108/mixxx_midi_clock
 *
 */

#include "MIDIUSB.h" // https://github.com/arduino-libraries/MIDIUSB (GNU LGPL)
#include <MIDI.h> // https://github.com/FortySevenEffects/arduino_midi_library (MIT)
#include <RotaryEncoder.h> // https://github.com/mathertel/RotaryEncoder (BSD)
#include <lcdgfx.h> // https://github.com/lexus2k/lcdgfx (MIT)

#define DEBUG 0

#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#else
#define debug(x)
#define debugln(x)
#endif

#define CONFIGURE_TIMER1(X) noInterrupts(); X; interrupts();

MIDI_CREATE_DEFAULT_INSTANCE();

const unsigned long CPU_FREQ = 16000000;
const unsigned long MICROS_PER_MIN = 60000000;
const int PPQ = 24;

const byte MIDI_START = 0xFA;
const byte MIDI_CONT = 0xFB;
const byte MIDI_STOP = 0xFC;
const byte MIDI_CLOCK = 0xF8;

const int CLOCK_MODE_SWITCH = 6;
int previousClockModeButtonState;
long lastClockModeButtonPressMs;

// free:             Ignores MIDI messages from Mixxx. Regular MIDI clock mode.
// ready:            Listening for MIDI messages from Mixxx.
// syncing:          Has receivd BPM and beat distance data from Mixxx.
// syncing_complete: MIDI clock set to begin on next beat in Mixxx.
// synced_to_mixxx:  MIDI clock started in sync to Mixxx.
enum class clockStatus {
  free,
  ready,
  syncing,
  syncing_complete,
  synced_to_mixxx
};
volatile enum clockStatus currentClockStatus = clockStatus::free;
volatile int currentClockPulse = 1; // Range 1..24 (PPQ).
volatile int barPosition = 1; // Range 1..96. Represents the position within a 4/4 measure.
int pausePosition;

bool receivingMidi = false;

float mixxxBPM = 0;
float freeClockBPM = 122;

const int PLAY_BUTTON = 7;
const int STOP_BUTTON = 4;

// started:  MIDI start messge will be sent at the beginning of the next 4/4 measure.
// playing:  MIDI start or continue sent.
// paused:   MIDI stop sent. 4/4 measure position where paused is cached.
// unpaused: MIDI continue will be sent. Play back will begin on the 4/4 measure position where previously paused.
// stopping: MIDI stop will be sent at the end of the current 4/4 measure.
// stopped:  MIDI stop sent. Bar position is not cached.
enum class playState {
  started,
  playing,
  paused,
  unpaused,
  stopping,
  stopped
};
enum playState currentPlayState = playState::stopped;

unsigned long lastBtnDebounceTimeMs = 0;
int debounceDelayMs = 200;

RotaryEncoder *jogKnob = nullptr;
const int JOG_KNOB_PIN1 = 9;
const int JOG_KNOB_PIN2 = 8;
const int JOG_KNOB_BUTTON = 5;

bool volatile tempoNudged = false;
int volatile tempoNudgedAtClockPulse = 0;
int volatile resumeFromTempoNudge = false;

DisplaySSD1306_128x64_I2C display(-1); // -1 means default I2C address (0x3C)
bool updateUIClockStatus = true;
bool updateUIPlayStatus = true;
bool updateUIBPM = true;

const int LED_BEAT_ONE = 13;
const int LED_BEAT_TWO = 12;
const int LED_BEAT_THREE = 11;
const int LED_BEAT_FOUR = 10;

// Store string constants in program memory to minimize RAM usage
const char clockStatusFree[] PROGMEM    = "Free   ";
const char clockStatusReady[] PROGMEM   = "Ready  ";
const char clockStatusSyncing[] PROGMEM = "Syncing";
const char clockStatusSynced[] PROGMEM  = "Synced ";
const char playStateStarted[] PROGMEM   = ">|";
const char playStatePlaying[] PROGMEM   = "|>";
const char playStatePaused[] PROGMEM    = "||";
const char playStateStopped[] PROGMEM   = "[]";
const char playStateStopping[] PROGMEM  = ">]";
const char splashMixxx[] PROGMEM        = "Mixxx";
const char splashMidiClock[] PROGMEM    = "MIDI Clock";

const char* const uiStringsTable[] PROGMEM = {
  clockStatusFree,
  clockStatusReady,
  clockStatusSyncing,
  clockStatusSynced,
  playStateStarted,
  playStatePlaying,
  playStatePaused,
  playStateStopped,
  playStateStopping,
  splashMixxx,
  splashMidiClock
};

void getStringFromTable(uint8_t index, char* buffer) {
  PGM_P p = (PGM_P)pgm_read_word(&(uiStringsTable[index]));
  strcpy_P(buffer, p);
}

void setup() {
  if (DEBUG) {
    Serial.begin(31250);
  }

  char buffer[11];

  display.begin();
  display.clear();
  display.setFixedFont(ssd1306xled_font6x8);
  display.setColor(1);
  getStringFromTable(9, buffer);
  display.printFixedN(34,  0, buffer, STYLE_NORMAL, FONT_SIZE_2X);
  getStringFromTable(10, buffer);
  display.printFixedN(5,  16, buffer, STYLE_NORMAL, FONT_SIZE_2X);

  MIDI.begin(MIDI_CHANNEL_OMNI);

  initializeTimer();

  pinMode(LED_BEAT_ONE, OUTPUT);
  pinMode(LED_BEAT_TWO, OUTPUT);
  pinMode(LED_BEAT_THREE, OUTPUT);
  pinMode(LED_BEAT_FOUR, OUTPUT);

  pinMode(CLOCK_MODE_SWITCH, INPUT);
  pinMode(JOG_KNOB_BUTTON, INPUT);
  pinMode(PLAY_BUTTON, INPUT);
  pinMode(STOP_BUTTON, INPUT);

  int previousClockModeButtonState = digitalRead(CLOCK_MODE_SWITCH);
  if (previousClockModeButtonState == HIGH) {
    currentClockStatus = clockStatus::ready;
  } else {
    currentClockStatus = clockStatus::free;
  }
  lastClockModeButtonPressMs = millis();

  jogKnob = new RotaryEncoder(JOG_KNOB_PIN1, JOG_KNOB_PIN2, RotaryEncoder::LatchMode::FOUR3);
  attachInterrupt(digitalPinToInterrupt(9), checkJogKnobPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(7), checkJogKnobPosition, CHANGE);

  delay(2000);

  display.clear();
}

void loop() {
  onSyncComplete();

  readMidiUSB();

  handlePlayButton();
  handleStopButton();

  onStart();
  onStop();
  onContinue();

  handleJogKnob();
  onResumeFromTempoNudge();

  pulseBPMLED();
  drawUI();

  handleClockModeButton();
}

void initializeTimer() {
  // Configure Timer1 for the default free clock BPM
  float intervalMicros = bpmToIntervalMicros(getBPM());
  unsigned long ocr = (CPU_FREQ * intervalMicros) / (8 * 1000000);
  CONFIGURE_TIMER1(
    TCCR1A = 0; // Control Register A
    TCCR1B = 0; // Control Register B (for setting prescaler and CTC mode)
    TCNT1  = 0; // initialize counter value to 0
    TCCR1B |= (0 << CS12) | (1 << CS11) | (0 << CS10); // Prescaler 8

    // Set tick that triggers the interrupt
    OCR1A = ocr;
    // Enable Clear Time on Compare match
    TCCR1B |= (1 << WGM12);

    // Enable timer overflow interrupt
    TIMSK1 |= (1 << OCIE1A);
   )
}

// Timer1 COMPA interrupt function
ISR(TIMER1_COMPA_vect) {
  sendMidiClock();

  // `syncing` that means the timer has just been configured by MIDI messages
  // received from Mixxx. This block therefore should only called on the first
  // timer interrupt function call, at which point that clock status is set to
  // `syncing_complete`. Then in the next loop function, the timer is set to the
  // PPQ interval.
  if (currentClockStatus == clockStatus::syncing) {
    currentClockStatus = clockStatus::syncing_complete;
    updateUIClockStatus = true;
  }

  // Keep track of the pulse count (PPQ) in range of 1..24
  currentClockPulse = (currentClockPulse % PPQ) + 1;
  barPosition = (barPosition % 96) + 1;

  if (tempoNudged) {
    // Uses modulo arithmetic to determine the clock pulse interval since the
    // nudge. It is constrainted to (under)overflows within range 1..24. The
    // modulo on a negative number is not treated like a positive number like it
    // is in other languages. For example, `-20 % 24 = 4`, while here it is
    // `-20`. This means PPQ is added to get rid of the negative, and value is
    // re-moduloed. The +/- 1 is because 1 is the minimum value.
    int clockPulsesSinceTempoNudged = ((currentClockPulse - tempoNudgedAtClockPulse - 1) \
                                         % PPQ + PPQ) % PPQ + 1;

    // 1/16th note (6 clock pulses) is the duration of a tempo nudge
    if (clockPulsesSinceTempoNudged >= 6) {
      resumeFromTempoNudge = true;
    }
  }
}

// Configure the timer with 24 PPQ intervalMicros based on BPM receivd from Mixxx
void onSyncComplete() {
  if (currentClockStatus == clockStatus::syncing_complete) {
    CONFIGURE_TIMER1(
      configureTimer(bpmToIntervalMicros(mixxxBPM));
      TCNT1  = 0; // reset Timer1 counter to 0
      currentClockStatus = clockStatus::synced_to_mixxx;
      updateUIClockStatus = true;
      updateUIBPM = true;
    )
  }
}

float bpmToIntervalMicros(float bpm) {
  return MICROS_PER_MIN / bpm / PPQ;
}

// Configure prescaler (1, 8, 64, 256, or 1024) based on the required PPQ
// interval for the given bpm's PPQ interval. Calculate the timer compare
// value for each prescaler to see if it fits in Timer1's 16 bits. The
// division 1M is to fix the units (eg, ms to s).
void configureTimer(float intervalMicros) {
  unsigned long ocr;
  byte tccr;

  if ((ocr = (CPU_FREQ * intervalMicros) / (1 * 1000000)) < 65535) {
    tccr |= (0 << CS12) | (0 << CS11) | (1 << CS10);
  } else if ((ocr = (CPU_FREQ * intervalMicros) / (8 * 1000000)) < 65535) {
    tccr |= (0 << CS12) | (1 << CS11) | (0 << CS10);
  } else if ((ocr = (CPU_FREQ * intervalMicros) / (64 * 1000000)) < 65535) {
    tccr |= (0 << CS12) | (1 << CS11) | (1 << CS10);
  } else if ((ocr = (CPU_FREQ * intervalMicros) / (256 * 1000000)) < 65535) {
    tccr |= (1 << CS12) | (0 << CS11) | (0 << CS10);
  } else if ((ocr = (CPU_FREQ * intervalMicros) / (1024 * 1000000)) < 65535) {
    tccr |= (1 << CS12) | (0 << CS11) | (1 << CS10);
  } else {
    // bpm is too slow. Exceeds timer's maxium interval, which is 4.19 seconds
    // (~14 bpm)
    return;
  }

  ocr = ocr - 1; // timer is 0 indexed

  if (ocr) {
    CONFIGURE_TIMER1(
      TCCR1B = 0; // Reset control register
      OCR1A = ocr; // Compare match value
      TCCR1B |= (1 << WGM12); // CTC mode
      TCCR1B |= tccr; // Prescaler
    )
  }
}

void readMidiUSB() {
  static int mixxxBPMWhole;
  static float mixxxBPMFractional;

  if (currentClockStatus != clockStatus::free) {
    midiEventPacket_t rx;

    do {
      rx = MidiUSB.read();
      if (rx.header != 0) {
        debug("Received: ");
        debug(rx.header);
        debug("-");
        debug(rx.byte1);
        debug("-");
        debug(rx.byte2);
        debug("-");
        debugln(rx.byte3);

        switch (rx.byte1 & 0xF0) {
        case 0xE0: {
              // Pitch bend carries the bpm data

              // The Mixxx controller script subtracts 60 from the BPM so it fits in a
              // 0-127 midi range. So, 60 is added to the value to get the actual BPM.
              // Supported BPM range: 60 - 187
              mixxxBPMWhole = rx.byte2 + 60;
              mixxxBPMFractional = rx.byte3 / 100.0;

              float oldMixxxBPM = mixxxBPM;
              mixxxBPM = mixxxBPMWhole + mixxxBPMFractional;
              if (mixxxBPM != oldMixxxBPM) {
                updateUIBPM = true;
                if (currentClockStatus == clockStatus::synced_to_mixxx) {
                  float intervalMicros = bpmToIntervalMicros(mixxxBPM);
                  configureTimer(intervalMicros);
                }
              }
            }
        case 0x90: {
            // Note On for note B8 carries the beat_distance
            if (rx.byte2 == 0x77 && !receivingMidi) {
              // beat_distance value from Mixxx is a number between 0 and 1. It
              // represents the distance from the previous beat marker. It is
              // multiplied by 127 in order to pass it as a midi value, so it is
              // divided here in order to get the original float value.

              // This assumes getting this message between beats 1 and 2 in a 4/4
              // measure and tries the guess when beat 2 will start.
              float beatDistance = 1 - (rx.byte3 / 127.0);
              float startAt = ((MICROS_PER_MIN / mixxxBPM) * beatDistance);

              CONFIGURE_TIMER1 (
                configureTimer(startAt);
                TCNT1  = 0; // reset Timer1 counter to 0
                currentClockStatus = clockStatus::syncing;
                currentClockPulse = 1;
                barPosition = PPQ + 1 // beat 2
              )
              receivingMidi = true;
              updateUIClockStatus = true;
            }
            break;
          }
        case 0x80: {
            // Note Off for note B8 indicates nothing is playing from Mixxx and
            // there is no sync leader
            if (rx.byte2 == 0x77) {
              receivingMidi = false;
              currentClockStatus = clockStatus::ready;
              updateUIClockStatus = true;
            }
            break;
          }
        default: {
            break;
          }
        }
      }
    } while (rx.header != 0);
  }
}

void sendMidiClock() {
  MIDI.sendRealTime(midi::Clock);
  midiEventPacket_t clockEvent ={0x0F, MIDI_CLOCK, 0x00, 0x00};
  MidiUSB.sendMIDI(clockEvent);
  MidiUSB.flush();
}

void sendMidiTransportMessage(byte message) {
  MIDI.sendRealTime(message);
  midiEventPacket_t transportEvent ={0x0F, message, 0x00, 0x00};
  MidiUSB.sendMIDI(transportEvent);
  MidiUSB.flush();
}

boolean buttonRising(int previousState, int currentState) {
  return previousState == LOW && currentState == HIGH;
}

void handlePlayButton() {
  static int previousButtonState = LOW;
  int buttonState = digitalRead(PLAY_BUTTON);

  if (buttonRising(previousButtonState, buttonState) && (millis() - lastBtnDebounceTimeMs) > debounceDelayMs) {
    switch (currentPlayState) {
    case playState::stopped:
      currentPlayState = playState::started; // Will start on beat 1
      break;
    case playState::playing:
      sendMidiTransportMessage(MIDI_STOP);
      currentPlayState = playState::paused;
      pausePosition = barPosition;
      break;
    case playState::paused:
      currentPlayState = playState::unpaused; // Will resume on pausePosition
      break;
    case playState::unpaused:
      currentPlayState = playState::paused; // Abort continue
      break;
    case playState::started:
      currentPlayState = playState::stopped; // Abort start playing
      break;
    case playState::stopping:
      currentPlayState = playState::playing; // Abort stop playing
      break;
    default:
      break;
    }
    lastBtnDebounceTimeMs = millis();
    updateUIPlayStatus = true;
  }
  previousPlayButtonState = buttonState;
}

void handleStopButton() {
  static int previousButtonState = LOW;
  int buttonState = digitalRead(STOP_BUTTON);

  if (buttonRising(previousButtonState, buttonState) && ((millis() - lastBtnDebounceTimeMs) > debounceDelayMs)) {
    currentPlayState = playState::stopping;
    lastBtnDebounceTimeMs = millis();
    updateUIPlayStatus = true;
  }
  previousStopButtonState = buttonState;
}

// Always start on beat 1
void onStart() {
  if (currentPlayState == playState::started && barPosition == 96) {
    sendMidiTransportMessage(MIDI_START);
    currentPlayState = playState::playing;
    updateUIPlayStatus = true;
  }
}

// Stop playback at the end of the current 4/4 measure
void onStop() {
  if (currentPlayState == playState::stopping && barPosition == 96) {
    sendMidiTransportMessage(MIDI_STOP);
    currentPlayState = playState::stopped;
    updateUIPlayStatus = true;
  }
}

// Resume playing at the same position in the bar when paused.
void onContinue() {
  if (currentPlayState == playState::unpaused && pausePosition == barPosition) {
    sendMidiTransportMessage(MIDI_CONT);
    currentPlayState = playState::playing;
    updateUIPlayStatus = true;
  }
}

// Interrupt function that updates the encoder's state
void checkJogKnobPosition() {
  jogKnob->tick();
}

// Encoder driven in order to mimic a DJ jog wheel as much as this is possible
// with a midi clock (eg, no going backwards). Used to nudge the tempo up and
// down temporarily in order to change the phase of the clock.
void handleJogKnob() {
  static int position = 0;

  jogKnob->tick();

  int newPosition = jogKnob->getPosition();

  if (position != newPosition) {
    switch (jogKnob->getDirection()) {
    case RotaryEncoder::Direction::CLOCKWISE:
      if (currentClockStatus == clockStatus::free && !digitalRead(JOG_KNOB_BUTTON)) {
        changeTempo(0.1);
      } else {
        nudgeTempo(0.9);
      }
      break;
    case RotaryEncoder::Direction::COUNTERCLOCKWISE:
      if (currentClockStatus == clockStatus::free && !digitalRead(JOG_KNOB_BUTTON)) {
        changeTempo(-0.1);
      } else {
        nudgeTempo(1.1);
      }
      break;
    default:
      // NOROTATION
      break;
    }
  }
}

void changeTempo(float amount) {
  float bpm = freeClockBPM + amount;
  freeClockBPM = max(60, min(bpm, 200));
  configureTimer(bpmToIntervalMicros(freeClockBPM));
  updateUIBPM = true;
}

// Temporary +/- adjustment to the current timer interval in order to speed up
// or slow down the midi clock. Used to adjust the phase of the clock.
void nudgeTempo(float amount) {
  float currentBPMInterval = bpmToIntervalMicros(getBPM());
  float nudgedInterval = currentBPMInterval * amount;
  configureTimer(nudgedInterval);
  tempoNudgedAtClockPulse = currentClockPulse;
  tempoNudged = true;
}

// Re-configures the timer after a tempo nudge back to the original interval
// based on the current BPM.
void onResumeFromTempoNudge() {
  if (resumeFromTempoNudge) {
    configureTimer(bpmToIntervalMicros(getBPM()));
    tempoNudged = false;
    resumeFromTempoNudge = false;
  }
}

float getBPM() {
  if (currentClockStatus == clockStatus::synced_to_mixxx) {
    return mixxxBPM;
  } else {
    return freeClockBPM;
  }
}

int bpmLEDPulseTime = 1;
void pulseBPMLED() {
  if (barPosition == 96) {
    bpmLEDPulseTime = 8;
    digitalWrite(LED_BEAT_ONE, HIGH);
  } else if (barPosition == 24) {
    bpmLEDPulseTime = 1;
    digitalWrite(LED_BEAT_TWO, HIGH);
  } else if (barPosition == 48) {
    bpmLEDPulseTime = 1;
    digitalWrite(LED_BEAT_THREE, HIGH);
  } else if (barPosition == 72) {
    bpmLEDPulseTime = 1;
    digitalWrite(LED_BEAT_FOUR, HIGH);
  } else if (!(currentClockPulse % bpmLEDPulseTime)) {
    digitalWrite(LED_BEAT_ONE, LOW);
    digitalWrite(LED_BEAT_TWO, LOW);
    digitalWrite(LED_BEAT_THREE, LOW);
    digitalWrite(LED_BEAT_FOUR, LOW);
  }
}

void drawUI() {
  static unsigned long lastDrawUIDebounceTimeMs = 0;
  if (updateUI && ((millis() - lastDrawUIDebounceTimeMs) > debounceDelayMs)) {
    if (updateUIBPM) {
      drawUIBPM();
      updateUIBPM = false;
    }
    if (updateUIClockStatus) {
      drawUIClockStatus();
      updateUIClockStatus = false;
    }
    if (updateUIPlayStatus) {
      drawUIPlayState();
      updateUIPlayStatus = false;
    }
    lastDrawUIDebounceTimeMs = millis();
  }
}

bool updateUI() {
  return updateUIClockStatus || updateUIPlayStatus || updateUIBPM;
}


void drawUIClockStatus() {
  int index;
  char buffer[8];
  switch (currentClockStatus) {
  case clockStatus::free:
    index = 0;
    break;
  case clockStatus::ready:
    index = 1;
    break;
  case clockStatus::syncing:
    index = 2;
    break;
  case clockStatus::syncing_complete:
    index = 2;
    break;
  case clockStatus::synced_to_mixxx:
    index = 3;
    break;
  default:
    break;
  }
  getStringFromTable(index, buffer);
  display.printFixedN(0, 0, buffer, STYLE_NORMAL, FONT_SIZE_2X);
}

void drawUIBPM() {
  char bpmString[7];
  dtostrf(getBPM(), 6, 2, bpmString);
  display.printFixedN(26, 24, bpmString, STYLE_NORMAL, FONT_SIZE_2X);
}

void drawUIPlayState() {
  int index;
  char buffer[3];

  switch (currentPlayState) {
  case playState::started:
    index = 4;
    break;
  case playState::playing:
    index = 5;
    break;
  case playState::paused:
    index = 6;
    break;
  case playState::unpaused:
    index = 4;
    break;
  case playState::stopping:
    index = 8;
    break;
  case playState::stopped:
    index = 7;
    break;
  default:
    break;
  }

  getStringFromTable(index, buffer);
  display.printFixedN(100, 0, buffer, STYLE_NORMAL, FONT_SIZE_2X);
}

void handleClockModeButton() {
  int buttonState = digitalRead(CLOCK_MODE_SWITCH);

  if (buttonState != previousClockModeButtonState && (millis() - lastClockModeButtonPressMs > 50)) {
    if (buttonState == HIGH) {
      currentClockStatus = clockStatus::ready;
    } else {
      currentClockStatus = clockStatus::free;
      receivingMidi = false;
      if (mixxxBPM) {
        freeClockBPM = mixxxBPM;
      }
    }
    updateUIClockStatus = true;
    previousClockModeButtonState = buttonState;
    lastClockModeButtonPressMs = millis();
  }
}

