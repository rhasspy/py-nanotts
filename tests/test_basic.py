import pytest


from py_nanotts import NanoTTS, Voice


def test_speak_returns_wav_bytes():
    tts = NanoTTS()
    data = tts.speak("Hello from NanoTTS", voice=Voice.en_GB)
    assert isinstance(data, (bytes, bytearray))
    assert data[:4] == b"RIFF"
    assert data[8:12] == b"WAVE"


def test_voice_string_accepted():
    tts = NanoTTS()
    data = tts.speak("Testing voice", voice="en-US")
    assert data[:4] == b"RIFF"


def test_speed_pitch_volume_clamped():
    tts = NanoTTS()
    data = tts.speak(
        "Clamping test",
        speed=999.0,
        pitch=0.0,
        volume=-5.0,
    )
    assert data[:4] == b"RIFF"
