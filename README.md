# Mixxx MIDI Clock
Ardunio based MIDI clock generator for [Mixxx](https://mixxx.org/). MIDI clock
can be used to sync external gear (sequencer, samplers, drum machinem etc...) to
tracks playing in Mixxx.

<img src="https://github.com/user-attachments/assets/e8a5072f-fe87-46cd-9184-a14144678f64" width="600"/>

## How does it work?
The accompanying [Mixxx controller script](https://github.com/apmiller108/mixxx_midi_clock/tree/main/mixxx) 
send two midi messages to the Arduino device at regular
intervals. One message contains the [BPM](https://manual.mixxx.org/2.4/en/chapters/appendix/mixxx_controls#control-[ChannelN]-bpm)
of the [Sync Leader](https://manual.mixxx.org/2.4/de/chapters/djing_with_mixxx#sync-lock-with-dynamic-tempo)
and the other message contains the 
[beat distance](https://manual.mixxx.org/2.4/en/chapters/appendix/mixxx_controls#control-[ChannelN]-beat_distance). 
The BPM data is used to create the MIDI clock and the beat distance data is used
to determine when the clock should start. While it is required that a Sync
Leader be set in order for the to produce a MIDI clock, it is not required that
sync be used on the other decks.

This device uses the beat distance data to guess where it should start sending
MIDI clock messages. For example, when pressing play (assuming on beat one) on
the Sync Leader deck, the clock will do it's best to guess when beat two will
land, and start the clock then. The device does not take into account your
system's audio latency; so, while the clock will be spot on in terms of tempo,
it will likely have been started slightly ahead of the Sync Leader. A couple
counter clock wise ticks on the encoder should get the beats lined up properly.
After that, it should remain in sync without further tweaking.

MIDI clock is sent over both USB and 5 Pin DIN.

MIDI is received from Mixxx over USB.

## Features
### Tempo sync to Mixxx Sync Leader deck
- Syncs to the tempo of the Sync Leader deck.
- Follows tempo changes made to the Sync Leader deck.
- Keeps track of the beat (ie, the position in a 4/4 measure). This allows for
  some sequencer like behavior (see [MIDI transport](#midi-transport)).
### Beat adjustment
- The encoder can be used to temporarily slow down or speed up the MIDI clock.
  This can be used to align the beats of the external gear with the track playing
  in Mixxx if needed. Alignment will need to be done when the clock first
  starts: the tempo will be matched, but the beat will be slightly offset.
- The Encoder's behavior tries in part to emulate a DJ jog as much as that is
  possible with a MIDI clock. It cannot make the gear play backwards, and there
  is a limit to how much it can speed up or slow down. It allows for aligning
  the beats however desired for whatever creative purpose.
### MIDI transport
There are two buttons that send MIDI Stop, Start and Continue messages to provide sequencer-like behavior:
#### Play / Pause button
- When stopped, pressing this button will wait until beat one begin playback.
- When playing, pressing this button will pause playback.
- When paused, pressing this button will resume playback from the beat position where it was paused.
#### Stop button
- Stops playback at the end of the current 4/4 measure. On the next press of the Play Button, playback will begin on beat one.
### OLED Display
- Clock Status
- Sequencer state (playing, paused, stopped, queued)
- BPM
### Beat LEDs
- An LED lights up on each quarter note beat.
### Master MIDI Clock
The device can be used independent of Mixxx as a master MIDI clock. Use the
Clock mode switch to toggle between Mixxx Clock and Free Clock modes.

In Free Clock Mode:
- Pressing and turning the encoder will change the tempo in increments of 0.01 BPM.
- Turning the encoder without pressing down will change the beat alignment (ie
  phase) just like in Mixxx Clock mode.
## Getting Started

1. Build the Arduino project. See [build instructions in the Wiki](https://github.com/apmiller108/mixxx_midi_clock/wiki/Building-the-Arduino-prototype).
2. Download the latest [release](https://github.com/apmiller108/mixxx_midi_clock/releases) to your computer (the `mixxx_midi_clock.ino.hex` file).
3. Upload the `mixxx_midi_clock.ino.hex` to the Arduino. See [upload instructions in the Wiki](https://github.com/apmiller108/mixxx_midi_clock/wiki/Uploading-the-code)
4. Move the controller script files to the `controllers` directory in your
  [Mixxx settings directory](https://manual.mixxx.org/2.4/en/chapters/appendix/settings_directory). 
   The script consists of two files:
    - `mixxx_midi_clock-script.js`
    - `mixxx_midi_click.midi.xml`
4. Use a MIDI cable to connect the Arduino to the gear that you want synced to
   Mixxx. In addition to the 5 pin DIN output, MIDI clock is also sent over USB.
   So you can also distribute the MIDI clock by routing it via your computer's
   audio backend.
5. Load the controller mapping (ie the controller script files) for the Arduino in Mixxx's 
   [controller settings](https://manual.mixxx.org/2.4/en/chapters/controlling_mixxx#using-midi-hid-controllers).
5. Load a track and set the deck to be a [Sync Leader](https://www.youtube.com/watch?v=REiG8DBKr-M). When pressing play on this deck, the clock will start.
6. Press the play button on the Arudino to start your external sequencer using MIDI transport (or just start your sequencer directly).
7. Make adjustments to the beat alignment using the encoder as needed. At first,
   the beat will likely be slightly ahead. A couple counter clockwise turns on
   the encounder should get the beats matched up. After that, there isn't much
   need to make adjustments. The clock will follow the sync leader tempo changes.
## Tested on
- Mixxx v2.4 running on Linux (pipewire), Arduino Leonardo and Roland T-8 drum machine
## Known Issues
When running on Linux with pipewire, occassionaly the clock seems to run a bit
fast for a given session. Every minute or so I need to turn the encoder counter
clockwise to keep it in sync. Restarting pipewire resolves it.

``` sh
systemctl --user restart wireplumber pipewire pipewire-pulse
```

## Disclaimer
- While this device works great for me (the author of this code), I can make no
guarantees that it will work for you.
- The design of this is highly opionated and built around my personal workflow and style.
- This is my first Arudino project and I'm probably doing some stupid things.

