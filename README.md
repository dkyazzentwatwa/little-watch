# Little Cube OS

Little Cube OS turns the Waveshare ESP32-S3-Touch-AMOLED-1.8 into a small,
calm personal information device. It is designed for quick glances and focused
actions, not for becoming a tiny smartphone.

The cube puts useful information close at hand:

- Check the time, weather, calendar, news, and daily overview.
- Read and write notes, browse files, and manage contacts.
- Record a voice note, review its transcription, save the approved text as a normal note, play audio, and read longer content.
- Listen to live MP3 internet radio from editable SD-card station presets.
- Ask the assistant questions using the cube's microphone and speaker.
- Use the touchscreen for navigation and USB serial when typing is useful.

Your content lives on the device's microSD card. Wi-Fi is used for connected
features such as weather, news, time updates, and assistant requests. The
device is intended to remain useful when those connections are unavailable.

## Hardware

This project is built for the **Waveshare ESP32-S3-Touch-AMOLED-1.8**, with a
368×448 AMOLED display, touchscreen, microphone, speaker, RTC, Wi-Fi, and
microSD storage. It is not currently a generic ESP32 application. Use the
exact board listed above.

## Try it

The firmware is built and installed with Arduino CLI. The complete setup and
hardware workflow is in [technical.md](technical.md).

```bash
./scripts/install-libraries.sh  # first time only
./scripts/build.sh
./scripts/upload.sh
./scripts/monitor.sh
```

The scripts require the board's FQBN and connected USB port. See the technical
guide before running them.

## Project status

Little Cube OS is under active development. Recording, playback, microphone
capture, and several assistant flows have been tested on the physical cube.
Other display, touch, SD-card, provisioning, and reliability behaviors remain
on the validation checklist. See [docs/hardware-validation.md](docs/hardware-validation.md)
for the current evidence instead of assuming that a successful build means a
feature is finished.

## Learn more

- [technical.md](technical.md) for setup, architecture, and development
- [docs/product-spec.md](docs/product-spec.md) for the product contract
- [docs/serial-interface.md](docs/serial-interface.md) for serial commands
- [docs/hardware-validation.md](docs/hardware-validation.md) for device testing
- [AGENTS.md](AGENTS.md) for contributor guidelines
