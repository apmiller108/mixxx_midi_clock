/*
 * mixxx_midi_clock.ino
 *
 * Created: 10/17/2024
 * Author: alex miller
 */

// TODO add button to toggle play pause.
// TODO add button to stop
// TODO add midi jack and write midi clock / transport controls to it
// TODO test with external drum machine
// TODO add encoder to change the phase
// TODO add screen to display bpm, phase offset, and transport state

#include "MIDIUSB.h"

const unsigned long CPU_FREQ = 16000000;  // 16 MHz clock speed
// 1 second in microseconds. This means the minimum supported BPM is 60 (eg, 1
// beat per 1 second)
const unsigned long MAX_CLOCK_TIME = 1000000UL;
const unsigned long MICROS_PER_MIN = 60000000UL;
const int PPQ = 24;
const float DEFAULT_BPM = 120; // Default BPM until read from midi messages from Mixxx

volatile float bpm = 0;
volatile unsigned int timerComparePulseValue;
volatile int currentClockPulse = 1;
volatile bool receivingMidi = false;

int bpmWhole;
float bpmFractional;
bool bpmChanged = false;

const int PLAY_BUTTON = 2; // pin 2
byte playState = 0; // 0 = stopped, 1 = playing, 2 = paused
int playButtonState;
unsigned long lastDebounceTimeMs = 0;
unsigned long debounceDelayMs = 50;

midiEventPacket_t rx;

void setup() {
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

  pinMode(PLAY_PAUSE_BUTTON, INPUT);
}

void loop() {
  // TODO refactor: extract this loop to a function
  do {
    rx = MidiUSB.read();
    if (rx.header != 0) {
      /* Serial.print("Received: "); */
      /* Serial.print(rx.header, HEX); */
      /* Serial.print("-"); */
      /* Serial.print(rx.byte1, HEX); */
      /* Serial.print("-"); */
      /* Serial.print(rx.byte2, HEX); */
      /* Serial.print("-"); */
      /* Serial.println(rx.byte3, HEX); */

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

        // TODO: try removing the stop / start here
        cli(); // stop interrupts
        bpm = newBpm;
        calculateTimerComparePulseValue();
        sei(); // resume interrupts

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
          cli(); // stop interrupts
          // Start next pulse when the next beat is predicted to happen
          OCR1A += beatLength * distToNextBeat;
          sei(); // resume interrupts
          receivingMidi = true;
        }
      }
    }
  } while (rx.header != 0);

  int playButtonState = digitalRead(PLAY_BUTTON);
  if (playButtonState == HIGH && ((millis() - lastDebounceTimeMs) > debounceDelayMs)) {
    if (playState == 0) { // stopped
      sendMidiStart();
    } else if (playState == 1) { // playing
      // TODO send stop message
    } else if (playState == 2) { // paused
      // TODO send continue
    }
    lastDebounceTimeMs = millis();
  }
}

// Timer1 COMPA interrupt function
ISR(TIMER1_COMPA_vect) {
  // Schedule the next interrupt
  OCR1A += timerComparePulseValue;

  sendMidiClock();

  // TODO: move the beat LED to loop function

  // Turn on LED on each beat for about 1/16th note duration
  if (currentClockPulse == 1) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (currentClockPulse == 6) {
    digitalWrite(LED_BUILTIN, LOW);
  }

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

// TODO refactor: extract sendMidiTransportMsg method
void sendMidiClock() {
  midiEventPacket_t clockEvent ={0x0F, 0xF8, 0x00, 0x00};
  MidiUSB.sendMIDI(clockEvent);
  MidiUSB.flush();
}

void sendMidiStart() {
  midiEventPacket_t clockEvent ={0x0F, 0xFA, 0x00, 0x00};
  MidiUSB.sendMIDI(clockEvent);
  MidiUSB.flush();
}
