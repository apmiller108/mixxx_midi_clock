/*
 * mixxx_midi_clock.ino
 *
 * Created: 10/17/2024
 * Author: alex miller
 * https://github.com/apmiller108/mixxx_midi_clock
 *
 */

// TODO UX: figure out what position to start at when first syncing to mixxx. Is it
// even possible? Maybe assume the first message comes on beat one active. But the
// first complete quarter note will be for beat 2. Actually, seems to think beat
// 1 is mixxx's beat 4.

#include <MIDI.h>       // https://github.com/FortySevenEffects/arduino_midi_library (MIT)
#include <USB-MIDI.h>   // https://github.com/lathoub/Arduino-USBMIDI (MIT)
#include <RotaryEncoder.h> // https://github.com/mathertel/RotaryEncoder (BSD)
#include <lcdgfx.h> // https://github.com/lexus2k/lcdgfx (MIT)

MIDI_CREATE_DEFAULT_INSTANCE();

USBMIDI_NAMESPACE::usbMidiTransport usbMIDI2();
MIDI_NAMESPACE::MidiInterface<USBMIDI_NAMESPACE::usbMidiTransport> MIDI2((USBMIDI_NAMESPACE::usbMidiTransport&)usbMIDI2);


#define DEBUG 0

#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Seriali.println(x)
#else
#define debug(x)
#define debugln(x)
#endif

#define CONFIGURE_TIMER1(X) noInterrupts(); X; interrupts();

const unsigned long CPU_FREQ = 16000000;
const unsigned long MICROS_PER_MIN = 60000000;
const int PPQ = 24;

const float DEFAULT_BPM = 122;

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

unsigned long lastBtnDebounceTimeMs = 0;
int debounceDelayMs = 200;

RotaryEncoder *jogKnob = nullptr;
bool volatile tempoNudged = false;
int volatile tempoNudgedAtClockPulse = 0;
int volatile resumeFromTempoNudge = false;

DisplaySSD1306_128x64_I2C display(-1); // -1 means default I2C address (0x3C)
bool updateUIClockStatus = true;
bool updateUIPlayStatus = true;
bool updateUIBPM = true;
unsigned long lastDrawUIDebounceTimeMs = 0;

midiEventPacket_t rx;

void setup() {
  /* Serial.begin(31250); */
  /* while(!Serial) { */

  display.begin();
  display.clear();
  display.setFixedFont(ssd1306xled_font6x8);
  display.setColor(1);
  display.printFixedN(34,  0, "Mixxx", STYLE_NORMAL, FONT_SIZE_2X);
  display.printFixedN(8,  16, "MIDI Clock", STYLE_NORMAL, FONT_SIZE_2X);

  delay(2000);

  display.clear();

  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.setHandleNoteOn(onNoteOn);

  initializeTimer();

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PLAY_BUTTON, INPUT);
  pinMode(STOP_BUTTON, INPUT);

  jogKnob = new RotaryEncoder(3, 7, RotaryEncoder::LatchMode::FOUR3);
  attachInterrupt(digitalPinToInterrupt(3), checkJogKnobPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(7), checkJogKnobPosition, CHANGE);
}

void loop() {
  onSyncComplete();

  MIDI2.read();

  handlePlayButton();
  handleStopButton();

  handleContinue();
  handleStart();

  handleJogKnob();
  handleResumeFromTempoNudged();

  handleBPMLED();
  drawUI();
}

void initializeTimer() {
  // Configure Timer1 for DEFAULT_BPM which uses a prescaler of 8
  float intervalMicros = bpmToIntervalMicros(DEFAULT_BPM);
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
    updateUIClockStatus = true;
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

    if (clockPulsesSinceTempoNudged >= 6) { // 1/16th note
      resumeFromTempoNudge = true;
    }
  }
}

// configure the timer with 24 ppq intervalMicros based on BPM receivd from Mixxx
void onSyncComplete() {
  if (currentClockStatus == clockStatus::syncing_complete) {
    configureTimer(bpmToIntervalMicros(mixxxBPM));
    currentClockStatus = clockStatus::synced_to_mixxx;
    updateUIClockStatus = true;
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

void onNoteOn(byte channel, byte note, byte velocity) {
  if (note == 0x34) {
    // The Mixxx controller script subtracts 50 from the BPM so it fits in a
    // 0-127 midi range. So, 50 is added to the value to get the actual BPM.
    mixxxBPMWhole = velocity + 50;
  }

  if (note == 0x35) {
    mixxxBPMFractional = velocity / 100.0;
  }

  float newMixxxBPM = mixxxBPMWhole + mixxxBPMFractional;
  if (newMixxxBPM != mixxxBPM && currentClockStatus == clockStatus::synced_to_mixxx) {
    mixxxBPM = newMixxxBPM;
    float intervalMicros = bpmToIntervalMicros(mixxxBPM);
    configureTimer(intervalMicros);
    updateUIBPM = true;
  }

  if (note == 0x32) {
    if (!receivingMidi) {
      // Start next pulse when the next beat is predicted to happen
      // This should only be needed once.

      // beat_distance value from Mixxx is a number between 0 and 1. It
      // represents the distance from the previous beat marker. It is
      // multiplied by 127 in order to pass it as a midi value, so it is
      // divided here in order to get the original float value.
      float beatDistance = 1 - (velocity / 127.0);
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

void sendMidiClock() {
  MIDI.sendRealTime(midi::Clock);
  MIDI2.sendRealTime(midi::Clock);
}

void sendMidiTransportMessage(byte message) {
  MIDI.sendRealTime(message);
  MIDI2.sendRealTime(message);
}

boolean playButtonRising() {
  return previousPlayButtonState == LOW && playButtonState == HIGH;
}

boolean stopButtonRising() {
  return previousStopButtonState == LOW && stopButtonState == HIGH;
}

void handlePlayButton() {
  playButtonState = digitalRead(PLAY_BUTTON);
  if (playButtonRising() && (millis() - lastBtnDebounceTimeMs) > debounceDelayMs) {
    switch (currentPlayState) {
    case playState::stopped:
      currentPlayState = playState::started; // Will start on beat 1
      break;
    case playState::playing:
      sendMidiTransportMessage(midi::Stop);
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
  previousPlayButtonState = playButtonState;
}

// Resume playing at the same position in the bar when paused.
void handleContinue() {
  if (currentPlayState == playState::unpaused && pausePosition == barPosition) {
    sendMidiTransportMessage(midi::Continue);
    currentPlayState = playState::playing;
    updateUIClockStatus = true;
  }
}

// Always start on beat 1
void handleStart() {
  if (currentPlayState == playState::started && barPosition == 96) {
    sendMidiTransportMessage(midi::Start);
    currentPlayState = playState::playing;
    updateUIPlayStatus = true;
  }
}

void handleStopButton() {
  stopButtonState = digitalRead(STOP_BUTTON);
  if (stopButtonRising() && ((millis() - lastBtnDebounceTimeMs) > debounceDelayMs)) {
    sendMidiTransportMessage(midi::Stop);
    currentPlayState = playState::stopped;
    shouldContinue = false;
    lastBtnDebounceTimeMs = millis();
    updateUIPlayStatus = true;
  }
  previousStopButtonState = stopButtonState;
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
      nudgeTempo(0.9);
      break;
    case RotaryEncoder::Direction::COUNTERCLOCKWISE:
      nudgeTempo(1.1);
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
void handleResumeFromTempoNudged() {
  if (resumeFromTempoNudge) {
    configureTimer(bpmToIntervalMicros(getBPM()));
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

int bpmLEDPulseTime = 1;
void handleBPMLED() {
  if (barPosition == 96) {
    bpmLEDPulseTime = 8;
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (currentClockPulse == 24) {
    bpmLEDPulseTime = 1;
    digitalWrite(LED_BUILTIN, HIGH);
  } else if (!(currentClockPulse % bpmLEDPulseTime)) {
    digitalWrite(LED_BUILTIN, LOW);
  }
}

void drawUI() {
  if (updateUI && ((millis() - lastDrawUIDebounceTimeMs) > debounceDelayMs)) {
    if (updateUIClockStatus) {
      drawUIClockStatus();
      updateUIClockStatus = false;
    }
    if (updateUIPlayStatus) {
      drawUIPlayState();
      updateUIPlayStatus = false;
    }
    if (updateUIBPM) {
      drawUIBPM();
      updateUIBPM = false;
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
