"""Python bindings for NanoTTS (SVOX Pico).

The `NanoTTS` class exposes a `speak()` method that returns raw PCM audio
(`16 kHz`, mono, 16-bit little-endian). This module also defines the supported
voices and parameter bounds used for input clamping.
"""

from enum import Enum
from typing import Optional, Union

MIN_SPEED = 0.2
MAX_SPEED = 5.0
MIN_PITCH = 0.5
MAX_PITCH = 2.0
MIN_VOLUME = 0.0
MAX_VOLUME = 5.0


class Voice(str, Enum):
    """Supported Pico voice identifiers."""
    en_GB = "en-GB"
    en_US = "en-US"
    de_DE = "de-DE"
    es_ES = "es-ES"
    fr_FR = "fr-FR"
    it_IT = "it-IT"


class NanoTTS:
    """Text-to-speech wrapper for NanoTTS.

    `speak()` returns raw PCM audio bytes with the following format:

    - sample rate: 16 kHz
    - channels: 1 (mono)
    - sample width: 16-bit little-endian
    """
    def speak(
        self,
        text: str,
        voice: Optional[Union[str, Voice]] = None,
        speed: Optional[float] = None,
        pitch: Optional[float] = None,
        volume: Optional[float] = None,
    ) -> bytes:
        """Synthesize text to raw PCM bytes.

        Args:
            text: Text to synthesize.
            voice: Voice identifier (e.g. "en-GB") or `Voice` enum.
            speed: Optional speed multiplier ($0.2$-$5.0$).
            pitch: Optional pitch multiplier ($0.5$-$2.0$).
            volume: Optional volume multiplier ($0.0$-$5.0$).

        Returns:
            Raw PCM audio bytes (16 kHz, mono, 16-bit LE).
        """
        if speed is not None:
            speed = max(MIN_SPEED, min(MAX_SPEED, speed))

        if pitch is not None:
            pitch = max(MIN_PITCH, min(MAX_PITCH, pitch))

        if volume is not None:
            volume = max(MIN_VOLUME, min(MAX_VOLUME, volume))

        if isinstance(voice, Voice):
            voice_str = voice.value
        else:
            voice_str = voice
        return _speak(
            text=text,
            voice=voice_str,
            speed=speed,
            pitch=pitch,
            volume=volume,
            lingware_path=_default_lingware_path(),
        )


def _default_lingware_path() -> str:
    """Resolve the installed lingware path.

    Prefers the packaged `py_nanotts/lang` directory if present, then falls
    back to the repository layout under `nanotts/lang`.
    """
    import os
    try:
        from importlib import resources
    except ImportError:  # pragma: no cover
        resources = None

    if resources is not None:
        try:
            package_root = resources.files("py_nanotts")
            lang_dir = package_root / "lang"
            if lang_dir.is_dir():
                return os.fspath(lang_dir)
        except Exception:
            pass

    base_dir = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(base_dir, "..", "nanotts", "lang"))


try:
    from ._nanotts import speak as _speak
except Exception as exc:  # pragma: no cover - import-time guard
    raise ImportError("NanoTTS native extension failed to load") from exc
