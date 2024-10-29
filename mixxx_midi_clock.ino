/*
 * mixxx_midi_clock.ino
 *
 * Created: 10/17/2024
 * Author: alex miller
 */

// TODO figure out what position to start at when first syncing to mixxx. Is it
// even possible? Maybe assume the first message comes on beat one active. But the
// first complete quarter note will be for beat 2.
// TODO Move the shouldContinue/Start to the playState enum
// TODO add encoder to change the phase
// TODO add screen to display bpm, phase offset, transport state and beat number in 4/4 time

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

#define CONFIGURE_TIMER1(X) cli(); X; sei();

const unsigned long CPU_FREQ = 16000000;  // 16 MHz clock speed
// 1 second in microseconds. This means the minimum supported BPM is 60 (eg, 1
// beat per 1 second)
const unsigned long MICROS_PER_MIN = 60000000;
const int PPQ = 24;
const float DEFAULT_BPM = 122; // Default BPM until read from midi messages from Mixxx

const byte MIDI_START = 0xFA;
const byte MIDI_CONT = 0xFB;
const byte MIDI_STOP = 0xFC;
const byte MIDI_CLOCK = 0xF8;

enum class clockStatus {
  free,
  syncing,
  syncing_complete,
  synced_to_mixxx
};
volatile enum clockStatus currentClockStatus = clockStatus::free;
volatile int currentClockPulse = 1;
volatile int position = 1; // Range 1..96. represents the position within a 4/4 measure.
int pausePostion;

bool receivingMidi = false;

float bpm = 0;
int bpmWhole;
float bpmFractional;

const int PLAY_BUTTON = 2;
const int STOP_BUTTON = 4;
enum class playState {
  playing,
  paused,
  stopped
};
enum playState currentPlayState = playState::stopped;
int playButtonState;
int stopButtonState;
int previousPlayButtonState = LOW;
int previousStopButtonState = LOW;
bool shouldContinue = false;
bool shouldStart = false;

unsigned long lastDebounceTimeMs = 0;
unsigned long debounceDelayMs = 75;

midiEventPacket_t rx;

void setup() {
  /* Serial.begin(31250); */
  MIDI.begin(MIDI_CHANNEL_OMNI);

  // Configure Timer1 for DEFAULT_BPM which uses a prescaler of 8
  float intervalMicros = bpmToIntervalUS(DEFAULT_BPM);
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

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PLAY_BUTTON, INPUT);
  pinMode(STOP_BUTTON, INPUT);
}

void loop() {
  if (currentClockStatus == clockStatus::syncing_complete) {
    // configure timer with 24 ppq intervalMicros based on BPM from Mixxx
    configureTimer(bpmToIntervalUS(bpm));
    currentClockStatus = clockStatus::synced_to_mixxx;
  }

  readMidiUSB();
  handlePlayButton();
  handleStopButton();
  handleContinue();
  handleStart();

  // Stop button
  if (currentClockPulse == 24) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (currentClockPulse == 6) {
    digitalWrite(LED_BUILTIN, LOW);
  }
}

// Timer1 COMPA interrupt function
ISR(TIMER1_COMPA_vect) {
  sendMidiClock();

  // When syncing that means the timer has been configured for the first beat
  // from Mixxx.
  if (currentClockStatus == clockStatus::syncing) {
    currentClockStatus = clockStatus::syncing_complete;
  }

  // Keep track of the pulse count (PPQ) in range of 1..24
  currentClockPulse = (currentClockPulse % PPQ) + 1;
  position = (position % 96) + 1;
}

float bpmToIntervalUS(float bpm) {
  return MICROS_PER_MIN / bpm / PPQ;
}

void configureTimer(float intervalMicros) {
  unsigned long ocr;
  byte tccr;

  // Configure prescaler (1, 8, 64, 256, or 1024) based on the required PPQ
  // interval for the given bpm. Calculate the timer compare value for each
  // prescaler to see if it fits in Timer1's 16 bits. The division 1M is to fix
  // the units (eg, ms to s).
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
    // bpm is too slow. Exceeds timer's maximum ticks.
    return;
  }

  ocr = ocr - 1; // timer is 0 indexed

  if (ocr) {
    CONFIGURE_TIMER1(
      TCCR1B = 0; // Reset control register
      OCR1A = ocr; // Peroid
      TCCR1B |= (1 << WGM12); // CTC mode
      TCCR1B |= tccr; // Prescaler
    )
  }
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
      if (newBpm != bpm && currentClockStatus == clockStatus::synced_to_mixxx) {
        bpm = newBpm;
        float intervalMicros = bpmToIntervalUS(bpm);
        configureTimer(intervalMicros);
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
          float beatDistance = 1 - (rx.byte3 / 127.0);
          float beatLength =  MICROS_PER_MIN / bpm;

          // Next tick starts in the in micro seconds to the next beat.
          // This assumes getting this message on beat_active for the first beat
          // in a measure. Maybe corrections can be made as needed by adjusting
          // the phase.
          float startIn = (beatLength * beatDistance);
          configureTimer(startIn);
          currentClockStatus = clockStatus::syncing;
          currentClockPulse = 1;
          position = 1;

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

void handlePlayButton() {
  playButtonState = digitalRead(PLAY_BUTTON);
  if (playButtonRising() && (millis() - lastDebounceTimeMs) > debounceDelayMs) {
    if (currentPlayState == playState::stopped) {
      shouldStart = true;
    } else if (currentPlayState == playState::playing) {
      sendMidiTransportMessage(MIDI_STOP);
      currentPlayState = playState::paused;
      pausePostion = position;
    } else if (currentPlayState == playState::paused) {
      shouldContinue = true;
    }
    lastDebounceTimeMs = millis();
  }
  previousPlayButtonState = playButtonState;
}

void handleContinue() {
  if (shouldContinue && position == pausePostion) {
    sendMidiTransportMessage(MIDI_CONT);
    currentPlayState = playState::playing;
    shouldContinue = false;
  }
}

void handleStart() {
  if (shouldStart && position == 96) {
    sendMidiTransportMessage(MIDI_START);
    currentPlayState = playState::playing;
    shouldStart = false;
  }
}

void handleStopButton() {
  stopButtonState = digitalRead(STOP_BUTTON);
  if (stopButtonRising() && ((millis() - lastDebounceTimeMs) > debounceDelayMs)) {
    sendMidiTransportMessage(MIDI_STOP);
    currentPlayState = playState::stopped;
    shouldContinue = false;
    lastDebounceTimeMs = millis();
  }
  previousStopButtonState = stopButtonState;
}
