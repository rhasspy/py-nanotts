from enum import Enum
from typing import Optional, Union

MIN_SPEED = 0.2
MAX_SPEED = 5.0
MIN_PITCH = 0.5
MAX_PITCH = 2.0
MIN_VOLUME = 0.0
MAX_VOLUME = 5.0


class Voice(str, Enum):
    en_GB = "en-GB"
    en_US = "en-US"
    de_DE = "de-DE"
    es_ES = "es-ES"
    fr_FR = "fr-FR"
    it_IT = "it-IT"


class NanoTTS:
    def speak(
        self,
        text: str,
        voice: Optional[Union[str, Voice]] = None,
        speed: Optional[float] = None,
        pitch: Optional[float] = None,
        volume: Optional[float] = None,
    ) -> bytes:
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
