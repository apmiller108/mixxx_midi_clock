/*
 * MIDIUSB_test.ino
 *
 * Created: 4/6/2015 10:47:08 AM
 * Author: gurbrinder grewal
 * Modified by Arduino LLC (2015)
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

float bpm;
int bpm_whole;
float bpm_fractional;
bool next_beat_in_set = false;
unsigned long previous_time = micros();
unsigned long next_beat_in;

unsigned long beat_led_previous_micros = 0;
const long beat_led_interval_micros = 100000;

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
        // The script the sends this data subtracts 50 from the BPM so it fits
        // in a 0-127 midi range. So, 50 is added to the value to get the actual BPM.
        bpm_whole = int(rx.byte3) + 50;
        /* Serial.print("BPM Whole: "); */
        /* Serial.println(bpm_whole); */
      }

      if (rx.byte2 == 0x35) {
        bpm_fractional = float(rx.byte3) / 100.0;
        /* Serial.print("BPM Fractional: "); */
        /* Serial.println(bpm_fractional); */
      }

      bpm = bpm_whole + bpm_fractional;
      /* Serial.print("BPM: "); */
      /* Serial.println(bpm); */

      if (rx.byte2 == 0x32 && (rx.byte1 & 0xF0) == 0x90) {
        unsigned long beat_length = ceil(60000000 / bpm);
        float beat_distance = rx.byte3 / 127.0;
        float dist_to_next_beat = 1 - beat_distance;
        if (!next_beat_in_set) {
          next_beat_in = ceil(beat_length * dist_to_next_beat);
          next_beat_in_set = true;
        }
        /* Serial.print("beat_length: "); */
        /* Serial.print(beat_length); */
        /* Serial.print(" beat_distance: "); */
        /* Serial.print(beat_distance); */
        /* Serial.print(" DTNB: "); */
        /* Serial.print(dist_to_next_beat); */
        /* Serial.print(" NBI: "); */
        /* Serial.println(next_beat_in); */
      }
    }
  } while (rx.header != 0);

  unsigned long current_time = micros();
  if (next_beat_in_set && (current_time - previous_time > next_beat_in)) {
    Serial.println("BEAT!!!");
    digitalWrite(LED_BUILTIN, HIGH);
    previous_time = current_time;
    beat_led_previous_micros = current_time;
  }

  if (current_time - beat_led_previous_micros > beat_led_interval_micros) {
    digitalWrite(LED_BUILTIN, LOW);
  }
}
