import pytest


from py_nanotts import NanoTTS, Voice


def test_speak_returns_pcm_bytes():
    tts = NanoTTS()
    data = tts.speak("Hello from NanoTTS", voice=Voice.en_GB)
    assert isinstance(data, (bytes, bytearray))
    assert len(data) > 0


def test_voice_string_accepted():
    tts = NanoTTS()
    data = tts.speak("Testing voice", voice="en-US")
    assert len(data) > 0


def test_speed_pitch_volume_clamped():
    tts = NanoTTS()
    data = tts.speak(
        "Clamping test",
        speed=999.0,
        pitch=0.0,
        volume=-5.0,
    )
    assert len(data) > 0
