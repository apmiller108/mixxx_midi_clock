/*
 * mixxx_midi_clock.ino
 *
 * Created: 10/17/2024
 * Author: alex miller
 */

// TODO test transport buttons
// TODO write midi clock / transport controls to serial midi jack
// TODO test with external gear
// TODO add encoder to change the phase
// TODO add screen to display bpm, phase offset, and transport state

#include "MIDIUSB.h"
#include <MIDI.h>

MIDI_CREATE_DEFAULT_INSTANCE();

#define DEBUG 0

#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Seriali.println(x)
#else
#define debug(x)
#define debugln(x)
#endif

const unsigned long CPU_FREQ = 16000000;  // 16 MHz clock speed
// 1 second in microseconds. This means the minimum supported BPM is 60 (eg, 1
// beat per 1 second)
const unsigned long MAX_CLOCK_TIME = 1000000;
const unsigned long MICROS_PER_MIN = 60000000;
const int PPQ = 24;
const float DEFAULT_BPM = 138.5; // Default BPM until read from midi messages from Mixxx

const byte MIDI_START = 0xFA;
const byte MIDI_CONT = 0xFB;
const byte MIDI_STOP = 0xFC;
const byte MIDI_CLOCK = 0xF8;

volatile unsigned int timerComparePulseValue;
volatile int currentClockPulse = 1;

// TODO: possible clock improvement
// initialize currentFlag to false
// initialize previousFlag to false

bool receivingMidi = false;

float bpm = 0;
int bpmWhole;
float bpmFractional;
bool bpmChanged = false;

const int PLAY_BUTTON = 2;
const int STOP_BUTTON = 4;
enum playState {
  PLAYING,
  PAUSED,
  STOPPED
};
enum playState currentPlayState = STOPPED;
int playButtonState;
int stopButtonState;
int previousPlayButtonState = LOW;
int previousStopButtonState = LOW;

unsigned long lastDebounceTimeMs = 0;
unsigned long debounceDelayMs = 75;

midiEventPacket_t rx;

void setup() {
  MIDI.begin(MIDI_CHANNEL_OMNI);
  // Set up Timer1
  TCCR1A = 0; // Control Register A
  TCCR1B = 0; // Control Register B
  TCCR1B |= B00000100; // Prescaler = 256

  // Compute the compare value (pulse length) and assign it to the Compare A
  // Register.
  calculateTimerComparePulseValue();
  OCR1A = timerComparePulseValue;

  TIMSK1 |= B00000010; // Enable timer overflow interrupt

  // Setup beat pulse LED. This LED will pulse on each beat (eg, first of every
  // 24 pulses).
  pinMode(LED_BUILTIN, OUTPUT);

  pinMode(PLAY_BUTTON, INPUT);
  pinMode(STOP_BUTTON, INPUT);
}

void loop() {
  readMidiUSB();

  // Play button
  playButtonState = digitalRead(PLAY_BUTTON);
  if (playButtonRising() && ((millis() - lastDebounceTimeMs) > debounceDelayMs)) {
    if (currentPlayState == STOPPED) {
      sendMidiTransportMessage(MIDI_START);
      currentPlayState = PLAYING;
    } else if (currentPlayState == PLAYING) {
      sendMidiTransportMessage(MIDI_STOP);
      currentPlayState = PAUSED;
    } else if (currentPlayState == PAUSED) {
      sendMidiTransportMessage(MIDI_CONT);
      currentPlayState = PLAYING;
    }
    lastDebounceTimeMs = millis();
  }
  previousPlayButtonState = playButtonState;

  // Stop button
  stopButtonState = digitalRead(STOP_BUTTON);
  if (stopButtonRising() && ((millis() - lastDebounceTimeMs) > debounceDelayMs)) {
    sendMidiTransportMessage(MIDI_STOP);
    currentPlayState = STOPPED;
    lastDebounceTimeMs = millis();
  }
  previousStopButtonState = stopButtonState;

  // Turn on LED on each beat for about 1/16th note duration
  if (currentClockPulse == 1) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (currentClockPulse == 6) {
    digitalWrite(LED_BUILTIN, LOW);
  }
}

// Timer1 COMPA interrupt function
ISR(TIMER1_COMPA_vect) {
  // Schedule the next interrupt
  OCR1A += timerComparePulseValue;

  sendMidiClock();

  // TODO: possible clock improvement
  // If the currentFlag is true or HAS_RISEN
  // set the previousFlag to currentFlag
  // Set the currentFlag to false

  // Keep track of the pulse count. Integer in the range 1..24.
  if (currentClockPulse < PPQ) {
    currentClockPulse++;
  } else if (currentClockPulse == PPQ) {
    currentClockPulse = 1;
  }
}

void calculateTimerComparePulseValue() {
  // Use the DEFAULT_BPM to generate the clock until bpm is set from midi
  // messages from Mixxx
  float currentBpm = (bpm > 0) ? bpm : DEFAULT_BPM;
  unsigned long pulsePeriod = (MICROS_PER_MIN / currentBpm) / PPQ;
  pulsePeriod = min(pulsePeriod, MAX_CLOCK_TIME);  // Ensure we don't exceed max clock time

  // Calculate the timer compare value
  // Timer clock = CPU clock / prescaler
  unsigned long timerClock = CPU_FREQ / 256;
  timerComparePulseValue = (pulsePeriod * timerClock) / 1000000UL;

  // Ensure the compare value fits in 16 bits for Timer1
  timerComparePulseValue = min(timerComparePulseValue, 65535);
}

void readMidiUSB() {
  // TODO: possible clock improvement
  // if the previousFlag was true but the currentFlag if false (FALLING or HAS_FALLEN)
  // 1. pause interrupts
  // 2. configure clock / prescaler based on bpm
  // 3. call calculateTimerComparePulseValue()
  // 4. reenable interrupts
  // 5. set previousFlag state to currentFlag
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

      if (rx.byte2 == 0x34) {
        // The Mixxx controller script subtracts 50 from the BPM so it fits in a
        // 0-127 midi range. So, 50 is added to the value to get the actual BPM.
        bpmWhole = rx.byte3 + 50;
      }

      if (rx.byte2 == 0x35) {
        bpmFractional = rx.byte3 / 100.0;
      }

      float newBpm = bpmWhole + bpmFractional;
      if (newBpm != bpm) {
        bpmChanged = true;

        bpm = newBpm;
        calculateTimerComparePulseValue();

        bpmChanged = false;
      }

      if (rx.byte2 == 0x32 && (rx.byte1 & 0xF0) == 0x90) {
        // The beat length for the given bpm in micros
        unsigned long beatLength =  MICROS_PER_MIN / bpm;

        // beat_distance value from Mixxx is a number between 0 and 1. It
        // represents the distance from the previous beat marker. It is
        // multiplied by 127 in order to pass it as a midi value, so it is
        // divided here in order to get the original float value.
        float beatDistance = rx.byte3 / 127.0;
        float distToNextBeat = 1 - beatDistance;
        if (!receivingMidi) {
          // Start next pulse when the next beat is predicted to happen
          // This should only be needed once.
          // TODO: determine if this is really needed. Maybe not if I can change
          // the phase manually which probably something I will need to do
          // anyway.
          // TODO: possible clock improvement
          // 1. pause interrupt
          // 2. configure prescaler based on the period
          // 3. set OCR1A
          // 4. unpause interrupt
          // 5. set currentFlag to true
          OCR1A += ((beatLength * distToNextBeat) * CPU_FREQ / 256) / 1000000UL;
          receivingMidi = true;
        }
      }
    }
  } while (rx.header != 0);
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

boolean playButtonRising() {
  return previousPlayButtonState == LOW && playButtonState == HIGH;
}

boolean stopButtonRising() {
  return previousStopButtonState == LOW && stopButtonState == HIGH;
}
