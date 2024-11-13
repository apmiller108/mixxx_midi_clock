/*
 *
 *  mixxx_midi_clock-script.js
 *
 *  Created: 10/17/2024
 *  Author: alex miller
 *  https://github.com/apmiller108/mixxx_midi_clock
 *
 *  Sends MIDI data to the Mixxx MIDI Clock Arduino hardware to produce MIDI
 *  clock signal that can sync exteral gear (eg, drum machine, sampler,
 *  sequencer, etc) to the Sync Leader deck in Mixxx
 *
 *  Compatible with Mixxx v2.4+
 */

const midiChannel = 11;
const messageInterval = 320; // Frequency in MS to send the midi messages
const debug = false;

class Deck {
  constructor(group) {
    this.group = group;

    // Set to 1 when deck is playing, but not when previewing.
    this.playLatched = engine.getValue(group, 'play_latched');
    engine.makeConnection(group, 'play_latched', (value) => {
      this.playLatched = value;
    });
  }

  // Returns a float. The real value BPM of the deck including rate adjustment.
  bpm() {
    return engine.getValue(this.group, "bpm");
  }

  // 0 = sync lock disabled, 1 = sync follower, 2 = sync leader.
  syncMode() {
    return engine.getValue(this.group, 'sync_mode');
  }

  // A value between 0..1. Relative position of the play marker between beat markers.
  beatDistance() {
    return engine.getValue(this.group, "beat_distance");
  }

  isSyncLeader() {
    return this.syncMode() == 2;
  }

  isSyncFollower() {
    return this.syncMode() == 1;
  }

  isPlaying() {
    return this.playLatched == 1;
  }
}

var mixxxMIDIClock = new class MixxxMIDIClock {
  constructor() {
    this.timer = 0;
    this.decks = [
      new Deck('[Channel1]'),
      new Deck('[Channel2]'),
      new Deck('[Channel3]'),
      new Deck('[Channel4]')
    ];
  }

  init() {
    this.timer = engine.beginTimer(messageInterval, this.sendMessage.bind(this));
  }

  sendMessage() {
    const syncLeader = this.findSyncLeader();
    const channel = midiChannel - 1;
    let syncFollower;
    let anyPlayingDeck;

    if (debug) {
      this.decks.forEach((d) => {
        console.log(d.group);
        console.log(d.syncMode());
        console.log(d.bpm());
        console.log(d.isPlaying());
        console.log(d.beatDistance());
      });
    }

    // If there is a sync leader and at least one deck is playing send Midi data
    // with BPM and playing deck beat distance over three messages.
    if (syncLeader) {
      let beatDistance;
      if (syncLeader.isPlaying()) {
        beatDistance = syncLeader.beatDistance();
      } else if (syncFollower = this.findSyncFollower() && syncFollower.isPlaying()) {
        beatDistance = syncFollower.beatDistance();
      } else if (anyPlayingDeck = this.decks.find(d => d.isPlaying())) {
        beatDistance = anyPlayingDeck.beatDistance();
      }
      if (beatDistance != undefined) {
        const bpmParts = syncLeader.bpm().toFixed(2).split('.').map(v => parseInt(v));
        // supports BPM in the range of 60..187
        const bpmWhole = Math.max(0, Math.min(127, bpmParts[0] - 60));
        const bpmFractional = bpmParts[1];

        // Pitch bend used send the bpm data in the MSB and LSB
        midi.sendShortMsg(0xE0 + channel, bpmWhole, bpmFractional);
        // On with note B8 (118)
        midi.sendShortMsg(0x90 + channel, 0x77, Math.round(beatDistance * 127));
      } else {
        this.sendOffMessage();
      }
    } else {
      this.sendOffMessage();
    }
  }

  findSyncLeader() {
    return this.decks.find(d => d.isSyncLeader());
  }

  findSyncFollower() {
    return this.decks.find(d => d.isSyncFollower() && d.isPlaying());
  }

  sendOffMessage() {
    const channel = midiChannel - 1;
    // Off with note B8 (118) and value of 0
    midi.sendShortMsg(0x80 + channel, 0x77, 0x0);
  }

  shutdown() {
    engine.stopTimer(this.timer);
    this.timer = 0;
    this.sendOffMessage();
  }
}

