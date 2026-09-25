#!/usr/bin/env python3
"""Convert externally produced phrases to device clips and refresh the Worker.

    python3 backend/convert_phrases.py [phrases] [data/clips] [--no-normalize]
        [--no-trim] [--max-pause=SECONDS]

Input names map to backend/topics.json: idN.* -> NNN.wav (topic N) and
fbN.* -> off_N.wav (fallback N). Any format ffmpeg reads is accepted. Output is
24 kHz mono IMA ADPCM WAV, at most ten seconds, loudness-normalized so replies
play at similar volume. Leading and trailing silence below -45 dB is trimmed;
--max-pause also shortens longer pauses inside a phrase. Requires ffmpeg on PATH.

It also rewrites the generated topic block in backend/worker.mjs from
backend/topics.json (topic descriptions and fallback count; answers stay local).
Deploy the Worker afterwards if that block changed.
"""
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FS_BYTES = 0x360000  # LittleFS partition in default_16MB.csv.
MAX_SECONDS = 10     # Firmware playback buffer limit.
SAMPLE_RATE = 24000
SILENCE = "-45dB"
EDGE = "start_periods=1:start_threshold=" + SILENCE + ":start_silence={keep}"
NAME = re.compile(r"^(id|fb)(\d+)$", re.IGNORECASE)


def clip_name(stem):
    match = NAME.match(stem)
    if not match:
        return None
    number = int(match.group(2))
    if match.group(1).lower() == "id":
        return f"{number:03d}" if 1 <= number <= 999 else None
    return f"off_{number}" if number >= 1 else None


def clip_seconds(path):
    """Decoded duration as the firmware computes it: fact, capped by the data."""
    data = path.read_bytes()
    pos, declared, capacity = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if tag == b"fact":
            declared = struct.unpack_from("<I", data, pos + 8)[0]
        elif tag == b"data":
            tail = size % 1024
            capacity = size // 1024 * 2041 + ((tail - 4) * 2 + 1 if tail else 0)
        pos += 8 + size + size % 2
    if capacity is None:
        raise ValueError(f"{path.name}: no data chunk")
    return min(capacity, declared or capacity) / SAMPLE_RATE


def update_worker(catalog):
    topics = catalog["topics"]
    ids = [t["id"] for t in topics]
    if len(set(ids)) != len(ids) or not all(isinstance(i, int) and 1 <= i <= 999 for i in ids):
        raise ValueError("topics.json: ids must be unique integers 1-999")
    rows = "".join(f"  {json.dumps([t['id'], t['topic']], ensure_ascii=False)},\n" for t in topics)
    block = (f"// BEGIN GENERATED TOPICS: edit backend/topics.json, then run backend/convert_phrases.py.\n"
             f"const TOPICS = [\n{rows}];\nconst FALLBACK_CLIPS = {len(catalog['fallbacks'])};\n"
             "// END GENERATED TOPICS")
    path = ROOT / "backend" / "worker.mjs"
    source = path.read_text(encoding="utf-8")
    marker = re.compile(r"// BEGIN GENERATED TOPICS[\s\S]*?// END GENERATED TOPICS")
    if not marker.search(source):
        raise ValueError("worker.mjs: generated topic markers missing")
    updated = marker.sub(lambda _: block, source)
    if updated != source:
        path.write_text(updated, encoding="utf-8")
        print("Updated backend/worker.mjs topics; redeploy the Worker.")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    normalize = "--no-normalize" not in sys.argv
    trim = "--no-trim" not in sys.argv
    pause = next((float(a.split("=", 1)[1]) for a in sys.argv if a.startswith("--max-pause=")), None)
    source = Path(args[0]) if args else ROOT / "phrases"
    output = Path(args[1]) if len(args) > 1 else ROOT / "data" / "clips"
    output.mkdir(parents=True, exist_ok=True)

    failed = False
    converted = set()
    for path in sorted(p for p in source.iterdir() if p.is_file()):
        name = clip_name(path.stem)
        if not name:
            print(f"skip {path.name}: expected idN.* or fbN.*")
            continue
        if name in converted:
            print(f"ERROR {path.name}: another file already produced {name}.wav")
            failed = True
            continue
        target = output / f"{name}.wav"
        filters = []
        if trim:
            # Trailing silence is trimmed by reversing; keep short natural edges.
            filters += [f"silenceremove={EDGE.format(keep=0.05)}", "areverse",
                        f"silenceremove={EDGE.format(keep=0.15)}", "areverse"]
        if pause is not None:
            filters.append(f"silenceremove=stop_periods=-1:stop_duration={pause}:"
                           f"stop_threshold={SILENCE}:stop_silence={pause}")
        if normalize:
            filters.append("loudnorm=I=-16:TP=-1.5:LRA=11")
        # ffmpeg drops a final partial ADPCM block; make that block padding only.
        filters.append("apad=pad_len=2041")
        command = ["ffmpeg", "-loglevel", "error", "-y", "-i", str(path), "-vn",
                   *(["-af", ",".join(filters)] if filters else []),
                   "-ar", str(SAMPLE_RATE), "-ac", "1", "-fflags", "+bitexact", "-map_metadata", "-1",
                   "-c:a", "adpcm_ima_wav", "-block_size", "1024", str(target)]
        if subprocess.run(command).returncode != 0:
            print(f"ERROR {path.name}: ffmpeg failed")
            failed = True
            continue
        seconds = clip_seconds(target)
        if seconds > MAX_SECONDS:
            target.unlink()
            print(f"ERROR {path.name}: {seconds:.1f} s exceeds {MAX_SECONDS} s; shorten it")
            failed = True
            continue
        converted.add(name)
        print(f"{path.name} -> {target.relative_to(ROOT) if target.is_relative_to(ROOT) else target}"
              f"  {seconds:.1f} s, {target.stat().st_size / 1024:.0f} KB")

    catalog = json.loads((ROOT / "backend" / "topics.json").read_text(encoding="utf-8"))
    update_worker(catalog)
    expected = [f"{t['id']:03d}" for t in catalog["topics"]]
    expected += [f"off_{i + 1}" for i in range(len(catalog["fallbacks"]))]
    missing = [n for n in expected if not (output / f"{n}.wav").exists()]
    if missing:
        print(f"Missing {len(missing)} of {len(expected)} clips: {', '.join(missing)}")

    used = sum(p.stat().st_size for p in (ROOT / "data").rglob("*") if p.is_file())
    print(f"data/ uses {used / 1048576:.2f} of {FS_BYTES / 1048576:.2f} MB "
          "(LittleFS needs some free space).")
    if used > FS_BYTES * 0.9:
        print("WARNING: data/ may not fit in LittleFS; shorten clips.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
