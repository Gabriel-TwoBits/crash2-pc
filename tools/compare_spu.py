#!/usr/bin/env python3
"""Compares the notes the PlayStation's SPU plays with the port's, from two logs of SPU register writes: the
PlayStation's from tools/spu_log.lua in PCSX-Redux, the port's from CRASH2_SPU_LOG (src/spu_host.cpp). A note is a
voice's key on: its sample's address, pitch, volumes and envelope then, until its key off, with the changes to its
volumes and pitch on the way. The two lists of notes, in the order they start, are aligned as a diff on their samples;
each note on one side only is listed, and each aligned pair whose start (against the pairs around it), length, pitch,
volumes, envelope or changes differ.

    python tools/compare_spu.py PS1.log PORT.log [--voices N] [--tolerance VOLUME] [--shown COUNT]

The music plays on the voices below SsSetReservedVoice's count (12 to 18 by level), the sound effects above: --voices
keeps the notes of the voices below N. The logs' clocks are their vertical blanks: the music ticks at each, the game's
commands (a piece started, stopped, faded) come when the game reaches them, so the aligned notes fall into runs of one
offset between the logs (a run is at least MIN_RUN notes long), and a note is late or early against its run's offset.
"""
import argparse
import difflib
import collections
import sys
from dataclasses import dataclass, field
from pathlib import Path

SPU_VOICE_BASE = 0x1F801C00
SPU_VOICE_STRIDE = 0x10
VOICES = 24
HALF_BITS = 16
KEY_ON_LOW = 0x1F801D88
KEY_ON_HIGH = 0x1F801D8A
KEY_OFF_LOW = 0x1F801D8C
KEY_OFF_HIGH = 0x1F801D8E
KEY_REGISTERS = {KEY_ON_LOW: (True, 0), KEY_ON_HIGH: (True, HALF_BITS), KEY_OFF_LOW: (False, 0),
                 KEY_OFF_HIGH: (False, HALF_BITS)}
# A voice's registers by their offset
VOLUME_LEFT = 0x0
VOLUME_RIGHT = 0x2
PITCH = 0x4
START = 0x6
ADSR_LOW = 0x8
ADSR_HIGH = 0xA
CHANGING = {VOLUME_LEFT: "left", VOLUME_RIGHT: "right", PITCH: "pitch"}
ADDRESS_SHIFT = 3
LOG_FIELDS = 3
HEXADECIMAL = 16
DEFAULT_TOLERANCE = 2
DEFAULT_SHOWN = 60
# A pitch within this of the other side's is the same (the PS1's rounding puts some 1 below)
PITCH_TOLERANCE = 1
MIN_RUN = 3
SHOWN_CHANGES = 4


@dataclass
class Note:
    voice: int
    on: int
    start: int
    pitch: int
    left: int
    right: int
    adsr: tuple
    off: int = None
    changes: list = field(default_factory=list)

    def describe(self):
        length = "held" if self.off is None else f"{self.off - self.on} long"
        return (f"blank {self.on} voice {self.voice}: sample 0x{self.start:05X} pitch 0x{self.pitch:04X} "
                f"volumes 0x{self.left:04X}/0x{self.right:04X}, {length}")


def read_log(path):
    """[(blank, address, value)] of a log's writes"""
    writes = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        words = line.split()
        if len(words) != LOG_FIELDS:
            sys.exit(f"{path}:{number}: not a write: {line!r}")
        else:
            writes.append((int(words[0]), int(words[1], HEXADECIMAL), int(words[2], HEXADECIMAL)))
    return writes


def voices_of(value, shift):
    return [voice for voice in range(VOICES) if (value << shift) >> voice & 1]


def notes(writes, voices):
    """The notes of the voices below voices, in the order they start. A write to a voice in the blank of its next key
    on sets that note up; the others while a note is held change it"""
    key_ons = set()
    for blank, address, value in writes:
        if address in KEY_REGISTERS and KEY_REGISTERS[address][0]:
            key_ons.update((voice, blank) for voice in voices_of(value, KEY_REGISTERS[address][1]))
        else:
            continue
    registers = {}
    held = {}
    found = []
    for blank, address, value in writes:
        registers[address] = value
        voice, offset = divmod(address - SPU_VOICE_BASE, SPU_VOICE_STRIDE)
        if address in KEY_REGISTERS:
            on, shift = KEY_REGISTERS[address]
            for keyed in voices_of(value, shift):
                if keyed >= voices:
                    continue
                elif on:
                    base = SPU_VOICE_BASE + keyed * SPU_VOICE_STRIDE
                    note = Note(keyed, blank, registers.get(base + START, 0) << ADDRESS_SHIFT,
                                registers.get(base + PITCH, 0), registers.get(base + VOLUME_LEFT, 0),
                                registers.get(base + VOLUME_RIGHT, 0),
                                (registers.get(base + ADSR_LOW, 0), registers.get(base + ADSR_HIGH, 0)))
                    if keyed in held and held[keyed].off is None:
                        held[keyed].off = blank
                    else:
                        # The voice was free, or released
                        pass
                    held[keyed] = note
                    found.append(note)
                elif keyed in held and held[keyed].off is None:
                    held[keyed].off = blank
                else:
                    # A key off for a voice already released
                    pass
        elif 0 <= voice < voices and offset in CHANGING and voice in held and held[voice].off is None and \
                (voice, blank) not in key_ons:
            held[voice].changes.append((blank - held[voice].on, CHANGING[offset], value))
        else:
            # Another register, or a voice not playing a note
            continue
    found.sort(key=lambda note: (note.on, note.start, note.pitch))
    return found


def run_offsets(pairs):
    """Each aligned pair's offset of its run: runs shorter than MIN_RUN take the offset of the run before them (or, at
    the start, after them)"""
    offsets = [port.on - ps1.on for ps1, port in pairs]
    runs = []
    for offset in offsets:
        if runs and runs[-1][0] == offset:
            runs[-1][1] += 1
        else:
            runs.append([offset, 1])
    long_runs = [offset for offset, length in runs if length >= MIN_RUN]
    current = long_runs[0] if long_runs else 0
    result = []
    for offset, length in runs:
        if length >= MIN_RUN:
            current = offset
        else:
            # A note or two late or early within the run around them
            pass
        result += [current] * length
    return result


def differences(ps1, port, start_offset, tolerance):
    """(kind, text) of what differs between two aligned notes, the port's start taken back by start_offset"""
    found = []
    if port.on - start_offset != ps1.on:
        found.append(("start", f"starts {port.on - start_offset - ps1.on:+d} blanks apart"))
    else:
        # Together
        pass
    if (ps1.off is None) != (port.off is None):
        found.append(("length", "held on one side only"))
    elif ps1.off is not None and ps1.off - ps1.on != port.off - port.on:
        found.append(("length", f"lasts {ps1.off - ps1.on} blanks on the PS1, {port.off - port.on} on the port"))
    else:
        # The same length, or held on both
        pass
    if abs(ps1.pitch - port.pitch) > PITCH_TOLERANCE:
        found.append(("pitch", f"pitch 0x{ps1.pitch:04X} on the PS1, 0x{port.pitch:04X} on the port"))
    else:
        # The same pitch
        pass
    if abs(ps1.left - port.left) > tolerance or abs(ps1.right - port.right) > tolerance:
        found.append(("volume", f"volumes 0x{ps1.left:04X}/0x{ps1.right:04X} on the PS1, "
                                f"0x{port.left:04X}/0x{port.right:04X} on the port"))
    else:
        # The same volumes
        pass
    if ps1.adsr != port.adsr:
        found.append(("envelope", f"envelope {ps1.adsr[0]:04X} {ps1.adsr[1]:04X} on the PS1, "
                                  f"{port.adsr[0]:04X} {port.adsr[1]:04X} on the port"))
    else:
        # The same envelope
        pass
    if len(ps1.changes) != len(port.changes) or any(
            a[:2] != b[:2] or abs(a[2] - b[2]) > tolerance for a, b in zip(ps1.changes, port.changes)):
        found.append(("changes", f"changes {ps1.changes[:SHOWN_CHANGES]} on the PS1, "
                                 f"{port.changes[:SHOWN_CHANGES]} on the port"))
    else:
        # The same changes
        pass
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ps1", type=Path, help="tools/spu_log.lua's log")
    parser.add_argument("port", type=Path, help="CRASH2_SPU_LOG's log")
    parser.add_argument("--voices", type=int, default=VOICES, help="keep the voices below this")
    parser.add_argument("--tolerance", type=int, default=DEFAULT_TOLERANCE, help="volumes this close are the same")
    parser.add_argument("--shown", type=int, default=DEFAULT_SHOWN, help="list at most this many differences")
    arguments = parser.parse_args()
    ps1 = notes(read_log(arguments.ps1), arguments.voices)
    port = notes(read_log(arguments.port), arguments.voices)
    matcher = difflib.SequenceMatcher(None, [note.start for note in ps1], [note.start for note in port],
                                      autojunk=False)
    report = []
    kinds = collections.Counter()
    aligned = 0
    differing = 0
    last_offset = None
    for kind, ps1_first, ps1_end, port_first, port_end in matcher.get_opcodes():
        if kind == "equal":
            pairs = list(zip(ps1[ps1_first:ps1_end], port[port_first:port_end]))
            aligned += len(pairs)
            for (ps1_note, port_note), offset in zip(pairs, run_offsets(pairs)):
                if offset != last_offset:
                    report.append(f"from PS1 blank {ps1_note.on}, the port's notes {offset:+d} blanks apart")
                    last_offset = offset
                else:
                    # The same run
                    pass
                found = differences(ps1_note, port_note, offset, arguments.tolerance)
                if found:
                    differing += 1
                    kinds.update(what for what, _ in found)
                    report.append(f"  {ps1_note.describe()}: {'; '.join(text for _, text in found)}")
                else:
                    continue
        elif kind in ("delete", "replace", "insert"):
            kinds["PS1 only"] += ps1_end - ps1_first
            kinds["port only"] += port_end - port_first
            report += [f"  PS1 only: {note.describe()}" for note in ps1[ps1_first:ps1_end]]
            report += [f"  port only: {note.describe()}" for note in port[port_first:port_end]]
        else:
            sys.exit(f"difflib: unknown opcode {kind}")
    print(f"{len(ps1)} notes on the PS1, {len(port)} on the port; {aligned} aligned, {differing} of them differing")
    print(", ".join(f"{kind}: {count}" for kind, count in kinds.most_common()))
    for line in report[:arguments.shown]:
        print(line)
    if len(report) > arguments.shown:
        print(f"... {len(report) - arguments.shown} more lines")
    else:
        # All listed
        pass


if __name__ == "__main__":
    main()
