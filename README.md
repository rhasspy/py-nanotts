# py-nanotts

Python bindings for NanoTTS (SVOX Pico). Generates raw PCM audio in a portable ABI3 wheel.

## Features

- Pure C extension using PicoTTS engine
- ABI3 / limited Python C API
- Returns raw PCM audio (16 kHz, mono, 16-bit LE)
- Bundles the Pico lingware files

## Install

Build a wheel locally:

```
python -m build --wheel
```

Install the resulting wheel from the `dist/` directory.

## Usage

```python
from py_nanotts import NanoTTS

pcm = NanoTTS().speak("Hello world", voice="en-GB", speed=1.0)
```

`NanoTTS.speak()` returns raw PCM bytes. Format:

- 16 kHz sample rate
- 1 channel (mono)
- 16-bit little-endian

## CLI

A minimal CLI is provided:

```
python -m py_nanotts "Hello world" -o hello.wav -v en-GB
```

## Notes

- Voices: `en-GB`, `en-US`, `de-DE`, `es-ES`, `fr-FR`, `it-IT`.
- The CLI writes a WAV file using Python's `wave` module.
