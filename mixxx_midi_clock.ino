/*
 * mixxx_midi_clock.ino
 *
 * Created: 10/17/2024
 * Author: alex miller
 */

#include "MIDIUSB.h"

// First parameter is the event type (0x09 = note on, 0x08 = note off).
// Second parameter is note-on/note-off, combined with the channel.
// Channel can be anything between 0-15. Typically reported to the user as 1-16.
// Third parameter is the note number (48 = middle C).
// Fourth parameter is the velocity (64 = normal, 127 = fastest).

void noteOn(byte channel, byte pitch, byte velocity) {
  midiEventPacket_t noteOn = {0x09, 0x90 | channel, pitch, velocity};
  MidiUSB.sendMIDI(noteOn);
}

void noteOff(byte channel, byte pitch, byte velocity) {
  midiEventPacket_t noteOff = {0x08, 0x80 | channel, pitch, velocity};
  MidiUSB.sendMIDI(noteOff);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
}

// First parameter is the event type (0x0B = control change).
// Second parameter is the event type, combined with the channel.
// Third parameter is the control number number (0-119).
// Fourth parameter is the control value (0-127).

void controlChange(byte channel, byte control, byte value) {
  midiEventPacket_t event = {0x0B, 0xB0 | channel, control, value};
  MidiUSB.sendMIDI(event);
}

void sendMidiClock() {
  midiEventPacket_t clockEvent ={0x0F, clockStatus, 0x00, 0x00};
  MidiUSB.sendMIDI(clockEvent);
  MidiUSB.flush();
}

const int ppq = 24;
const byte clockStatus = 0xF8;

float bpm;
int bpmWhole;
float bpmFractional;
unsigned long previousTime = micros();
unsigned long startClockIn;
bool  clockStartInSet = false;
bool runClock = false;

long clockPulseInterval;
long currentClockPulseInterval;
int currentClockPulse = 1;

unsigned long beatLedPreviousMicros = 0;
const long beatLedIntervalMicros = 100000;

midiEventPacket_t rx;

// TODO: send 24 ppq
// TODO: send clock to ardour to verify accurate BPM
// TODO: Use drum machine to guage latency
// TODO: detect BPM changes and recalculate interval

void loop() {
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
        bpmWhole = int(rx.byte3) + 50;
        /* Serial.print("BPM Whole: "); */
        /* Serial.println(bpmWhole); */
      }

      if (rx.byte2 == 0x35) {
        bpmFractional = float(rx.byte3) / 100.0;
        /* Serial.print("BPM Fractional: "); */
        /* Serial.println(bpmFractional); */
      }

      bpm = bpmWhole + bpmFractional;
      /* Serial.print("BPM: "); */
      /* Serial.println(bpm); */

      if (rx.byte2 == 0x32 && (rx.byte1 & 0xF0) == 0x90) {
        // The beat length for the given bpm in micros
        // TODO: drop the use of ceil in this block
        unsigned long beatLength = ceil(60000000 / bpm);
        clockPulseInterval = ceil(60000000 / (ppq * bpm));

        // beat_distance value from Mixxx is a number between 0 and 1. It
        // represents the distance from the previous beat marker. It is
        // multiplied by 127 in order to pass it as a midi value, so it is
        // divided here in order to get the original float value.
        float beatDistance = rx.byte3 / 127.0;
        float distToNextBeat = 1 - beatDistance;
        if (!runClock) {
          currentClockPulseInterval = ceil(beatLength * distToNextBeat);
          runClock = true;
        }
        /* Serial.print("beatLength: "); */
        /* Serial.print(beatLength); */
        /* Serial.print(" beatDistance: "); */
        /* Serial.print(beatDistance); */
        /* Serial.print(" DTNB: "); */
        /* Serial.print(distToNextBeat); */
        /* Serial.print(" NBI: "); */
        /* Serial.println(startClockIn); */
      }
    }
  } while (rx.header != 0);

  unsigned long currentTime = micros();
  if (runClock && (currentTime - previousTime > currentClockPulseInterval)) {
    sendMidiClock();

    previousTime = currentTime;

    // Turn on LED on each beat
    if (currentClockPulse == 1) {
      digitalWrite(LED_BUILTIN, HIGH);
      beatLedPreviousMicros = currentTime;
    }

    // Keep track of the pulse count
    if (currentClockPulse < ppq) {
      currentClockPulse++;
    } else if (currentClockPulse == ppq) {
      currentClockPulse = 1;
    }

    // reset the pulse interval back to interval computed from the bpm
    currentClockPulseInterval = clockPulseInterval;
  }

  // Turn off beat LED after 1/16 note of time
  if (currentTime - beatLedPreviousMicros > (clockPulseInterval * 6)) {
    digitalWrite(LED_BUILTIN, LOW);
  }
}
