# Mixxx MIDI Clock
Ardunio based midi clock generator for [Mixxx](https://mixxx.org/). MIDI clock
can be used to sync external gear (sequencer, samplers, drum machinem etc...) to
tracks playing in Mixxx.

## How does it work?
The controller script send two midi messages to the Arduino device at regular
intervals. One message contains the BPM of the [Sync Leader](https://manual.mixxx.org/2.4/de/chapters/djing_with_mixxx#sync-lock-with-dynamic-tempo)
and the other message contains the beat distance. The BPM data is used to create
the MIDI clock and the beat distance data is used to determine when the clock
should start. While it is required that a Sync Leader be set in order for the
device to produce a MIDI clock, it is not required that sync be used on the
other decks.

This device uses the beat distance data to guess where it should start sending
MIDI clock messages. For example, when pressing play (assuming on beat one) on
the Sync Leader deck, the clock will do it's best to guess when beat two will
land and start the clock then. The device does not take into account your
system's audio latency; so, while the clock will be spot on in terms of tempo,
it will likely have been started slightly ahead of the Sync Leader. A couple
counter clock wise ticks on the encoder should get the beats lined up properly.
After that, it should remain in sync without further tweaking.

## Features
### Tempo sync to Mixxx Sync Leader deck
### Beat adjustment (ie, phase)
### MIDI transport
There are two buttons that send MIDI Stop, Start and Continue messages to provide sequencer-like behavior.
#### Play / Pause button
- When stopped, pressing this button will wait until beat one begin playback.
- When playing, pressing this button will pause playback.
- When paused, pressing this button will resume playback from the beat position where it was paused.
#### Stop button
- Stops playback. On the next press of the Play Button, playback will begin on beat one.
### Master MIDI Clock
The device can be used independent of Mixxx as a master MIDI clock. Use the Clock mode switch to toggle between Mixxx Clock and Free Clock modes.
- Pressing and turning the encoder will change the Tempo in increments of 0.01 BPM.
- Turning the encoder without pressing down will change the beat alignment (ie phase).
## Getting Started
## Known Issues
