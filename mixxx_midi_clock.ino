/*
 * mixxx_midi_clock.ino
 *
 * Created: 10/17/2024
 * Author: alex miller
 */

// TODO dynaically configure the clock
// TODO When pressing play, stop interrupts, send play msg, send midi clock, reset clock, set period and restart interrupts
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

#define CONFIGURE_TIMER1(X) noInterrupts(); X; interrupts()

const unsigned long CPU_FREQ = 16000000;  // 16 MHz clock speed
// 1 second in microseconds. This means the minimum supported BPM is 60 (eg, 1
// beat per 1 second)
const unsigned long MICROS_PER_MIN = 60000000;
const int PPQ = 24;
const float DEFAULT_BPM = 138.5; // Default BPM until read from midi messages from Mixxx

const byte MIDI_START = 0xFA;
const byte MIDI_CONT = 0xFB;
const byte MIDI_STOP = 0xFC;
const byte MIDI_CLOCK = 0xF8;

unsigned int intervalUS;
volatile int currentClockPulse = 1;

bool receivingMidi = false;

byte clockStatus = 0; // 0 free, 1 syncing to mixxx, 2 syncing to mixxx complete, 3 synced to mixxx

float bpm = 0;
int bpmWhole;
float bpmFractional;

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

  // Configure Timer1 for DEFAULT_BPM which uses a prescaler of 8
  bpmToIntervalUS(DEFAULT_BPM);
  unsigned int ocr = ((CPU_FREQ * intervalUS / (8 * 1000000)) + 0.5)
  CONFIGURE_TIMER1(
    TCCR1A = 0; // Control Register A
    TCCR1B = 0; // Control Register B
    TCNT1  = 0; // initialize counter value to 0
    TCCR1B |= (0 << CS12) | (1 << CS11) | (0 << CS10); // Prescaler 8

    // Set tick that triggers the interrupt
    OCR1A = ocr;
    // Enable Clear Time on Compare match
    TCCR1B |= (1 << WGM12);

    // Enable timer overflow interrupt
    TIMSK1 |= (1 << OCIE1A);
  );

  pinMode(LED_BUILTIN, OUTPUT);

  pinMode(PLAY_BUTTON, INPUT);
  pinMode(STOP_BUTTON, INPUT);
}

void loop() {
  if (clockStatus == 2) { // SYNC COMPLETE
    configureTimer(intervalUS); // configure timer with 24 ppq intervalUS based on BPM from Mixxx
    clockStatus = 3; // SYNCED
  }

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

  // TODO Consider longer blinks on the 1 beat of each measure (assume 4/4). Keeps a count of 1..96.
  // Turn on LED on each beat for about 1/16th note duration
  if (currentClockPulse == 24) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (currentClockPulse == 6) {
    digitalWrite(LED_BUILTIN, LOW);
  }
}

// Timer1 COMPA interrupt function
ISR(TIMER1_COMPA_vect) {
  sendMidiClock();

  if (clockStatus == 1) { // SYNCING
    clockStatus == 2; // SYNC COMPLETE
  }

  // Keep track of the pulse count (PPQ)
  if (currentClockPulse < PPQ) {
    currentClockPulse++;
  } else if (currentClockPulse == PPQ) {
    currentClockPulse = 1;
  }
}

float bpmToIntervalUS(float bpm) {
  intervalUS = MICROS_PER_MIN / bpm) / PPQ;
}

void configureTimer(float intervalUS) {
  unsigned int ocr;
  byte tccr;

  // Configure prescaler (1, 8, 64, 256, or 1024) based on the required PPQ
  // interval for the given bpm. Calculate the timer compare value for each
  // prescaler to see if it fits in Timer1's 16 bytes. MS to S via * 1M. Adds
  // 0.5 to ensure conversion to int rounds up or down appropriately.
  if ((ocr = ((CPU_FREQ * intervalUS / (1 * 1000000)) + 0.5)) < 65535) {
    tccr |= (0 << CS12) | (0 << CS11) | (1 << CS10);
  } else if ((ocr = ((CPU_FREQ * intervalUS / (8 * 1000000)) + 0.5)) < 65535) {
    tccr |= (0 << CS12) | (1 << CS11) | (0 << CS10);
  } else if ((ocr = ((CPU_FREQ * intervalUS / (64 * 1000000)) + 0.5)) < 65535) {
    tccr |= (0 << CS12) | (1 << CS11) | (1 << CS10);
  } else if ((ocr = ((CPU_FREQ * intervalUS / (256 * 1000000)) + 0.5)) < 65535) {
    tccr |= (1 << CS12) | (0 << CS11) | (0 << CS10);
  } else if ((ocr = ((CPU_FREQ * intervalUS / (1024 * 1000000)) + 0.5)) < 65535) {
    tccr |= (1 << CS12) | (0 << CS11) | (1 << CS10);
  } else {
    // bpm is slow. Exceeds timer's maximum ticks.
    return;
  }

  CONFIGURE_TIMER1(
    TCCR1B = 0;
    OCR1A = ocr;
    TCCR1B |= (1 << WGM12);
    TCCR1B |= tccr;
  )
}

void readMidiUSB() {
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
        bpm = newBpm;
        bpmToIntervalUS();
      }

      if (clockStatus == 3) {
        configureTimer(intervalUS);
      }

      if (rx.byte2 == 0x32 && (rx.byte1 & 0xF0) == 0x90) {
        if (!receivingMidi) {
          // TODO: determine if this is really needed. Maybe not if I can change
          // the phase manually which probably something I will need to do
          // anyway.


          // Start next pulse when the next beat is predicted to happen
          // This should only be needed once.

          // beat_distance value from Mixxx is a number between 0 and 1. It
          // represents the distance from the previous beat marker. It is
          // multiplied by 127 in order to pass it as a midi value, so it is
          // divided here in order to get the original float value.
          float beatDistance = 1 - (rx.byte3 / 127.0)
          // The beat length for the given bpm in micros
          unsigned long beatLength =  MICROS_PER_MIN / bpm;

          // Next tick starts in the the time in US to get to the next beat minus 24 ticks
          unsigned int startIn = (beatLength * beatDistance) - (24 * intervalUS);
          configureTimer(startIn);
          currentClockPulse = 1;
          clockStatus = 1; // SYNCING

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
