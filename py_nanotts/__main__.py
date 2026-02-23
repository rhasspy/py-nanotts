"""Command-line interface for NanoTTS.

Example:
    python -m py_nanotts "Hello world" -o hello.wav -v en-GB --speed 0.9
"""

import argparse
import wave

from . import MAX_PITCH, MAX_SPEED, MAX_VOLUME, MIN_PITCH, MIN_SPEED, MIN_VOLUME, NanoTTS, Voice


def _parse_voice(value: str) -> str:
    """Return a canonical voice string if it matches the enum."""
    try:
        return Voice(value).value
    except Exception:
        return value


def main() -> int:
    """Parse CLI args, synthesize PCM, and write a WAV file."""
    parser = argparse.ArgumentParser(description="NanoTTS CLI")
    parser.add_argument("text", help="Text to synthesize")
    parser.add_argument("-o", "--output", required=True, help="Output WAV file")
    parser.add_argument("-v", "--voice", default=None, help="Voice (e.g. en-GB)")
    parser.add_argument("--speed", type=float, default=None, help=f"Speed ({MIN_SPEED}-{MAX_SPEED})")
    parser.add_argument("--pitch", type=float, default=None, help=f"Pitch ({MIN_PITCH}-{MAX_PITCH})")
    parser.add_argument("--volume", type=float, default=None, help=f"Volume ({MIN_VOLUME}-{MAX_VOLUME})")

    args = parser.parse_args()

    tts = NanoTTS()
    pcm = tts.speak(
        args.text,
        voice=_parse_voice(args.voice) if args.voice else None,
        speed=args.speed,
        pitch=args.pitch,
        volume=args.volume,
    )

    with wave.open(args.output, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(16000)
        wf.writeframes(pcm)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
