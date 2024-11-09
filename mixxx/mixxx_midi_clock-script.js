/*
    mixxx_midi_clock

    Sends MIDI data to the Mixxx MIDI Clock Arduino hardware to produce MIDI
    clock signal that can sync exteral gear (eg, drum machine, sampler,
    sequencer, etc) to the Sync Leader deck in Mixxx
*/

const sysexBegin = 0xF0;
const sysexEnd = 0xF7;
const deviceId = 0x7A;

class MixxxMIDIClock {
  timer = 0;
  decks = [
    new Deck('[Channel1]'),
    new Deck('[Channel2]'),
    new Deck('[Channel3]'),
    new Deck('[Channel4]')
  ];

  static get sysexBegin() {
    return sysexBegin;
  }

  static get sysexEnd() {
    return sysexEnd;
  }

  static get deviceId() {
    return deviceId;
  }

  static get offMessage() {
    retrurn [MixxxMIDIClock.sysexBegin, MixxxMIDIClock.deviceId, 0, 0, 0, MixxxMIDIClock.sysexEnd];
  }

  init() {
    this.timer = engine.beginTimer(375, this.sendMessage.bind(this));
  }

  sendMessage() {
    const syncLeader = findSyncLeader();
    let syncFollower;
    let anyPlayingDeck;
    let message;

    const bpmParts = syncLeader.bpm().toFixed(2).toString().split('.').map(v => parseInt(v));
    const bpmWhole = Math.max(0, Math.min(127, parts[0] - 60)); // supports BPM in the range of 60..187
    const bpmFractional = parts[1];

    let beatDistance;
    if (syncLeader && syncLeader.isPlaying()) {
      beatDistance = syncLeader.beatDistance();
    } else if (syncFollower = this.findSyncFollower() && syncFollower.isPlaying()) {
      beatDistance = syncFollower.beatDistance();
    } else if (anyPlayingDeck = this.decks.find(d => d.isPlaying())) {
      beatDistance = anyPlayingDeck.beatDistance();
    }

    // If there is a sync leader and at least one deck is playing send bpm message
    if (syncLeader && beatDistance != undefined) {
      message = [
        MixxxMIDIClock.sysexBegin,
        MixxxMIDIClock.deviceId,
        bpmWhole,
        bpmFractional,
        beatDistance,
        MixxxMIDIClock.sysexEnd
      ];
    } else {
      message = MixxxMIDIClock.offMessage;
    }
    midi.sendSysexMsg(message, message.length);
  }

  findSyncLeader() {
    return this.decks.find(d => d.isSyncLeader());
  }

  findSyncFollower() {
    return this.decks.find(d => d.isSyncFollower() && d.isPlaying());
  }

  shutdown() {
    const message = MixxxMIDIClock.offMessage;
    midi.sendSysexMsg(message, message.length);
    engine.stopTimer(this.timer);
    this.timer = 0;
  }
}

class Deck {
  group;

  constructor(group) {
    this.group = group;
  }

  // A float, precision beyond 2 decimal places.
  bpm() {
    return engine.getValue(this.group, "bpm");
  }

  // 0 = sync lock disabled, 1 = sync follower, 2 = sync leader.
  syncMode() {
    return engine.getValue(this.group, "sync_mode");
  }

  isSyncLeader() {
    return this.syncMode() == 2;
  }

  isSyncFollower() {
    return this.syncMode() == 1;
  }

  // A value between 0..1. Relative position of the play marker between beat markers.
  beatDistance() {
    return engine.getValue(this.group, "beat_distance");
  }

  // Set to 1 when deck is playing, but not when previewing.
  playLatched() {
    return engine.getValue(this.group, "play_latched");
  }

  isPlaying() {
    return this.playLatched() == 1;
  }
}

const mixxxMIDIClock = new MixxxMIDIClock();
