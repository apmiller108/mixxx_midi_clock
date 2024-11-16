/*
 * mixxx_midi_clock.ino
 *
 * Author: alex miller
 * https://github.com/apmiller108/mixxx_midi_clock
 *
 */

// TODO feature: add switch to keep clock in free mode
// TODO feature: add pot to adjust bpm when in free clock mode

#include "MIDIUSB.h" // https://github.com/arduino-libraries/MIDIUSB (GNU LGPL)
#include <MIDI.h> // https://github.com/FortySevenEffects/arduino_midi_library (MIT)
#include <RotaryEncoder.h> // https://github.com/mathertel/RotaryEncoder (BSD)
#include <lcdgfx.h> // https://github.com/lexus2k/lcdgfx (MIT)

MIDI_CREATE_DEFAULT_INSTANCE();

#define DEBUG 0

#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#else
#define debug(x)
#define debugln(x)
#endif

#define CONFIGURE_TIMER1(X) noInterrupts(); X; interrupts();

const unsigned long CPU_FREQ = 16000000;
const unsigned long MICROS_PER_MIN = 60000000;
const int PPQ = 24;

const float freeClockBPM = 122;

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
int mixxxBPMWhole;
float mixxxBPMFractional;

const int PLAY_BUTTON = 8;
const int STOP_BUTTON = 4;
enum class playState {
  started,
  playing,
  paused,
  unpaused,
  stopped
};
enum playState currentPlayState = playState::stopped;
int previousPlayButtonState = LOW;
int previousStopButtonState = LOW;

unsigned long lastBtnDebounceTimeMs = 0;
int debounceDelayMs = 200;

RotaryEncoder *jogKnob = nullptr;
bool volatile tempoNudged = false;
int volatile tempoNudgedAtClockPulse = 0;
int volatile resumeFromTempoNudge = false;
const int JOG_KNOB_BUTTOM = 9999;
int previousJogKnobButtonState = LOW;
unsigned long lastJogKnobBtnDebouceTime = 0;

DisplaySSD1306_128x64_I2C display(-1); // -1 means default I2C address (0x3C)
bool updateUIClockStatus = true;
bool updateUIPlayStatus = true;
bool updateUIBPM = true;
unsigned long lastDrawUIDebounceTimeMs = 0;

midiEventPacket_t rx;

const int LED_BEAT_ONE = 13;
const int LED_BEAT_TWO = 12;
const int LED_BEAT_THREE = 11;
const int LED_BEAT_FOUR = 10;

void setup() {
  /* Serial.begin(31250); */
  display.begin();
  display.clear();
  display.setFixedFont(ssd1306xled_font6x8);
  display.setColor(1);
  display.printFixedN(34,  0, "Mixxx", STYLE_NORMAL, FONT_SIZE_2X);
  display.printFixedN(5,  16, "MIDI Clock", STYLE_NORMAL, FONT_SIZE_2X);

  MIDI.begin(MIDI_CHANNEL_OMNI);

  initializeTimer();

  pinMode(LED_BEAT_ONE, OUTPUT);
  pinMode(LED_BEAT_TWO, OUTPUT);
  pinMode(LED_BEAT_THREE, OUTPUT);
  pinMode(LED_BEAT_FOUR, OUTPUT);

  pinMode(CLOCK_MODE_SWITCH, INPUT);
  pinMode(JOG_KNOB_BUTTOM, INPUT);
  pinMode(PLAY_BUTTON, INPUT);
  pinMode(STOP_BUTTON, INPUT);

  int previousClockModeButtonState = digitalRead(CLOCK_MODE_SWITCH);
  if (previousClockModeButtonState == HIGH) {
    currentClockStatus = clockStatus::ready;
  } else {
    currentClockStatus = clockStatus::free;
  }
  lastClockModeButtonPressMs = millis();

  jogKnob = new RotaryEncoder(3, 7, RotaryEncoder::LatchMode::FOUR3);
  attachInterrupt(digitalPinToInterrupt(3), checkJogKnobPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(7), checkJogKnobPosition, CHANGE);

  delay(2000);

  display.clear();
}

void loop() {
  onSyncComplete();

  readMidiUSB();

  handlePlayButton();
  handleStopButton();

  onContinue();
  onStart();

  handleJogKnob();
  onResumeFromTempoNudge();

  pulseBPMLED();
  drawUI();

  handleClockModeButton();

  if (clockStatus == clockStatus::synced_to_mixxx) {
    int buttonState = digitalRead(JOG_KNOB_BUTTOM);
    if (buttonRising(JOG_KNOB_BUTTOM, buttonState) && (millis() - lastJogKnobBtnDebouceTime > debounceDelayMs)) {
      CONFIGURE_TIMER1 (
        configureTimer(bpmToIntervalMicros(mixxxBPM));
        TCNT1  = 0;
        currentClockPulse = 24;
        barPosition = 96
      )
      lastJogKobButtonPressed = millis();
    }
   previousJogKnobButtonState = buttonState;
  }
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
  if (currentClockStatus != clockStatus::free) {
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

boolean buttonRising(int button, int currentState) {
  switch (button) {
  case PLAY_BUTTON: {
    return previousPlayButtonState == LOW && currentState == HIGH;
  }
  case STOP_BUTTON: {
    return previousStopButtonState == LOW && currentState == HIGH;
  }
  case JOG_KNOB_BUTTOM: {
    return previousJogKnobButtonState == LOW && currentState == HIGH;
  }
  default:
    break;
  }
}

void handlePlayButton() {
  inst buttonState = digitalRead(PLAY_BUTTON);
  if (buttonRising(PLAY_BUTTON, buttonState) && (millis() - lastBtnDebounceTimeMs) > debounceDelayMs) {
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
    default:
      break;
    }
    lastBtnDebounceTimeMs = millis();
    updateUIPlayStatus = true;
  }
  previousPlayButtonState = buttonState;
}

// Resume playing at the same position in the bar when paused.
void onContinue() {
  if (currentPlayState == playState::unpaused && pausePosition == barPosition) {
    sendMidiTransportMessage(MIDI_CONT);
    currentPlayState = playState::playing;
    updateUIPlayStatus = true;
  }
}

// Always start on beat 1
void onStart() {
  if (currentPlayState == playState::started && barPosition == 96) {
    sendMidiTransportMessage(MIDI_START);
    currentPlayState = playState::playing;
    updateUIPlayStatus = true;
  }
}

void handleStopButton() {
  int buttonState = digitalRead(STOP_BUTTON);
  if (buttonRising(STOP_BUTTON, buttonState) && ((millis() - lastBtnDebounceTimeMs) > debounceDelayMs)) {
    sendMidiTransportMessage(MIDI_STOP);
    currentPlayState = playState::stopped;
    lastBtnDebounceTimeMs = millis();
    updateUIPlayStatus = true;
  }
  previousStopButtonState = buttonState;
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
      if (clockStatus == clockStatus::free && digitalRead(JOG_KNOB_BUTTOM)) {
        freeClockBPM = Math.min(240, freeClockBPM + 0.1)
        configureTimer(bpmToIntervalMicros(freeClockBPM));
      } else {
        nudgeTempo(0.9);
      }
      break;
    case RotaryEncoder::Direction::COUNTERCLOCKWISE:
      if (clockStatus == clockStatus::free && digitalRead(JOG_KNOB_BUTTOM)) {
        freeClockBPM = Math.max(60, freeClockBPM - 0.1)
        configureTimer(bpmToIntervalMicros(freeClockBPM));
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
  char* clockStatus;
  switch (currentClockStatus) {
  case clockStatus::free:
    clockStatus = "Free   ";
    break;
  case clockStatus::ready:
    clockStatus = "Ready  ";
    break;
  case clockStatus::syncing:
    clockStatus = "Syncing";
    break;
  case clockStatus::syncing_complete:
    clockStatus = "Syncing";
    break;
  case clockStatus::synced_to_mixxx:
    clockStatus = "Synced ";
    break;
  default:
    clockStatus = "       ";
    break;
  }
  display.printFixedN(0, 0, clockStatus, STYLE_NORMAL, FONT_SIZE_2X);
}

void drawUIBPM() {
  char bpmString[7];
  dtostrf(getBPM(), 6, 2, bpmString);
  display.printFixedN(26, 24, bpmString, STYLE_NORMAL, FONT_SIZE_2X);
}

void drawUIPlayState() {
  char* icon;
  switch (currentPlayState) {
  case playState::started:
    icon = ">|";
    break;
  case playState::playing:
    icon = "|>";
    break;
  case playState::paused:
    icon = "||";
    break;
  case playState::unpaused:
    icon = ">|";
    break;
  case playState::stopped:
    icon = "[]";
    break;
  default:
    icon = "";
    break;
  }
  display.printFixedN(100, 0, icon, STYLE_NORMAL, FONT_SIZE_2X);
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
        freeClockBPM = mixxxBPM
      }
    }
    updateUIClockStatus = true;
    previousClockModeButtonState = buttonState;
    lastClockModeButtonPressMs = millis();
  }
}

