import threading
import time
import wave

import pytest

from py_nanotts import NanoTTS, Voice, _speak


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


def test_genfile_markup_does_not_write_file(tmp_path):
    target = tmp_path / "written.wav"
    tts = NanoTTS()
    data = tts.speak(f'<genfile file="{target}">Hello</genfile>', voice="en-US")
    assert len(data) > 0
    assert not target.exists()


def test_play_markup_does_not_read_file(tmp_path):
    source = tmp_path / "source.wav"
    with wave.open(str(source), "wb") as wav_file:
        wav_file.setframerate(16000)
        wav_file.setsampwidth(2)
        wav_file.setnchannels(1)
        wav_file.writeframes(b"\x00\x40" * 16000 * 5)

    text = f'one <play file="{source}"/> two'
    tts = NanoTTS()
    played = tts.speak(text, voice="en-US")

    # Markup is spoken as plain text instead of splicing in the file
    assert played == tts.speak(text.replace("<", " "), voice="en-US")


def test_speak_releases_gil():
    tts = NanoTTS()
    text = "This sentence is long enough to keep the engine busy. " * 40
    done = threading.Event()

    def synthesize():
        tts.speak(text, voice="en-US")
        done.set()

    thread = threading.Thread(target=synthesize)
    iterations = 0
    thread.start()
    while not done.is_set():
        iterations += 1
        time.sleep(0)
    thread.join()

    # With the GIL held for the whole call, this loop barely runs
    assert iterations > 100


def test_unsupported_voice():
    with pytest.raises(ValueError):
        NanoTTS().speak("Hello", voice="xx-XX")


def test_missing_lingware(tmp_path):
    with pytest.raises(RuntimeError):
        _speak(text="Hello", voice="en-US", lingware_path=str(tmp_path))
