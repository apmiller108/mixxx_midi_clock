
/*
 * mixxx_midi_clock.ino
 *
 * Created: 10/17/2024
 * Author: alex miller
 */

// TODO UX: figure out what position to start at when first syncing to mixxx. Is it
// even possible? Maybe assume the first message comes on beat one active. But the
// first complete quarter note will be for beat 2. Actually, seems to think beat
// 1 is mixxx's beat 4.
// TODO Feature add screen to display bpm, phase offset, transport state and beat number in 4/4 time
// TODO Refactor: change US to Micros. Be consistent.

#include "MIDIUSB.h"    // https://github.com/arduino-libraries/MIDIUSB (GNU LGPL)
#include <MIDI.h>       // https://github.com/FortySevenEffects/arduino_midi_library (MIT)
#include <NewEncoder.h> // https://github.com/gfvalvo/NewEncoder (MIT?)

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

const unsigned long CPU_FREQ = 16000000; // 16 MHz clock speed
const unsigned long MICROS_PER_MIN = 60000000;
const int PPQ = 24;

const float DEFAULT_BPM = 122;

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
int playButtonState;
int stopButtonState;
int previousPlayButtonState = LOW;
int previousStopButtonState = LOW;
bool shouldContinue = false;

unsigned long lastDebounceTimeMs = 0;
unsigned long debounceDelayMs = 200;

NewEncoder jogKnob(3, 7, -100, 100, 0, FULL_PULSE);
long previousJogKnobValue;
bool volatile tempoNudged = false;
int volatile tempoNudgedAtClockPulse = 0;
int volatile tempoNudgedClockPulseInterval = 6; // Tempo nudges lasts for 1/16th note.
int volatile resumeFromTempoNudge = false;

const int SCREEN_WIDTH = 128; 
const int SCREEN_HEIGHT = 64; 
const int OLED_RESET = -1;
const byte SCREEN_ADDRESS = 0x3C;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

midiEventPacket_t rx;

void setup() {
  /* Serial.begin(31250); */
  /* while(!Serial) { */
  /* } */
  MIDI.begin(MIDI_CHANNEL_OMNI);
  jogKnob.begin();

  initializeTimer();

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PLAY_BUTTON, INPUT);
  pinMode(STOP_BUTTON, INPUT);

  // TODO get a display up and running
  //   See also https://www.instructables.com/Arduino-and-the-SSD1306-OLED-I2C-128x64-Display/

  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }

  display.display();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.print(F("mixxx midi clock"));
  display.display();

  delay(2000);

  display.clearDisplay();
  display.display();

  /* Draw a single pixel in white */
  /* display.drawPixel(10, 10, SSD1306_WHITE); */
  /* display.display(); */
}

void loop() {
  onSyncComplete();

  readMidiUSB();

  handlePlayButton();
  handleStopButton();

  handleContinue();
  handleStart();

  handleJogKnob();
  handleResumeFromTempoNudged();

  handleBPMLED();
  if (currentClockPulse == 24) {
    handleDrawUI();
  }
}

void handleDrawUI() {
  display.setCursor(0, 16);
  display.setTextSize(1);
  display.print(F("BPM: "));
  display.print(getBPM());
  display.display();
}

void initializeTimer() {
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
}

// Timer1 COMPA interrupt function
ISR(TIMER1_COMPA_vect) {
  sendMidiClock();

  // Syncing that means the timer has been configured for Mixxx's next beat.
  if (currentClockStatus == clockStatus::syncing) {
    currentClockStatus = clockStatus::syncing_complete;
  }

  // Keep track of the pulse count (PPQ) in range of 1..24
  currentClockPulse = (currentClockPulse % PPQ) + 1;
  barPosition = (barPosition % 96) + 1;

  if (tempoNudged) {
    // Uses modulo arithmetic to determine the clock pulse interval since the
    // nudge. It is constrainted to (under)overflows within range 1..24. In CPP
    // the modulo on a negative number is not treated like a positive number
    // like it is in other languages. In Ruby, for example, `-20 % 24 = 4`,
    // while in CPP it is `-20`. This means PPQ is added to get rid of the
    // negative if present, and value is re-modulo-ed. The +/- 1 is because 1
    // is the minimum value.
    int clockPulsesSinceTempoNudged = ((currentClockPulse - tempoNudgedAtClockPulse - 1) \
                                         % PPQ + PPQ) % PPQ + 1;

    if (clockPulsesSinceTempoNudged >= tempoNudgedClockPulseInterval) {
      resumeFromTempoNudge = true;
    }
  }
}

// configure the timer with 24 ppq intervalMicros based on BPM receivd from Mixxx
void onSyncComplete() {
  if (currentClockStatus == clockStatus::syncing_complete) {
    configureTimer(bpmToIntervalUS(mixxxBPM));
    currentClockStatus = clockStatus::synced_to_mixxx;
  }
}

float bpmToIntervalUS(float bpm) {
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
        mixxxBPMWhole = rx.byte3 + 50;
      }

      if (rx.byte2 == 0x35) {
        mixxxBPMFractional = rx.byte3 / 100.0;
      }

      float newMixxxBPM = mixxxBPMWhole + mixxxBPMFractional;
      if (newMixxxBPM != mixxxBPM && currentClockStatus == clockStatus::synced_to_mixxx) {
        mixxxBPM = newMixxxBPM;
        float intervalMicros = bpmToIntervalUS(mixxxBPM);
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
          float beatLength =  MICROS_PER_MIN / mixxxBPM;

          // Next tick starts in the in micro seconds to the next beat.
          // This assumes getting this message on beat_active for the first beat
          // in a measure. Maybe corrections can be made as needed by adjusting
          // the phase.
          float startIn = (beatLength * beatDistance);
          configureTimer(startIn);
          currentClockStatus = clockStatus::syncing;
          currentClockPulse = 1;
          barPosition = 1;

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
    lastDebounceTimeMs = millis();
  }
  previousPlayButtonState = playButtonState;
}

// Resume playing at the same position in the bar when paused.
void handleContinue() {
  if (currentPlayState == playState::unpaused && pausePosition == barPosition) {
    sendMidiTransportMessage(MIDI_CONT);
    currentPlayState = playState::playing;
  }
}

// Always start on beat 1
void handleStart() {
  if (currentPlayState == playState::started && barPosition == 96) {
    sendMidiTransportMessage(MIDI_START);
    currentPlayState = playState::playing;
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

// Encoder driven in order to mimic a DJ jog wheel as much as this is possible
// with a midi clock (eg, no going backwards). Used to nudge the tempo up and
// down temporarily in order to change the phase of the clock.
void handleJogKnob() {
  long currentValue;
  NewEncoder::EncoderState currentState;

  jogKnob.getState(currentState);
  currentValue = currentState.currentValue;

  if (previousJogKnobValue != currentValue) {
    if (currentValue > previousJogKnobValue) {
      nudgeTempo(0.9);
    } else {
      nudgeTempo(1.10);
    }
    previousJogKnobValue = currentValue;
  } else if (currentState.currentClick == NewEncoder::UpClick) {
    nudgeTempo(0.9);
  } else if (currentState.currentClick == NewEncoder::DownClick) {
    nudgeTempo(1.10);
  }
}

// Temporary +/- adjustment to the current timer interval in order to speed up
// or slow down the midi clock. Used to adjust the phase of the clock.
void nudgeTempo(float amount) {
  float currentBPMIntervalUS = bpmToIntervalUS(getBPM());
  float nudgedInterval = currentBPMIntervalUS * amount;
  configureTimer(nudgedInterval);
  tempoNudgedAtClockPulse = currentClockPulse;
  tempoNudged = true;
}

// Re-configures the timer after a tempo nudge back to the original interval
// based on the current BPM.
void handleResumeFromTempoNudged() {
  if (resumeFromTempoNudge) {
    configureTimer(bpmToIntervalUS(getBPM()));
    tempoNudged = false;
    resumeFromTempoNudge = false;
  }
}

float getBPM() {
  if (currentClockStatus == clockStatus::synced_to_mixxx) {
    return mixxxBPM;
  }
  return DEFAULT_BPM;
}

void handleBPMLED() {
  if (currentClockPulse == 24) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (currentClockPulse == 6) {
    digitalWrite(LED_BUILTIN, LOW);
  }
}
