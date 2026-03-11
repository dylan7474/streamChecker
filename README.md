# streamChecker

`streamChecker` is a small Linux desktop internet radio player written in C with GTK and GStreamer.
It lets you:

- Search public radio stations by name or tag (via Radio Browser API)
- Play selected streams in-app
- Download the currently playing stream to a local file

## Build on Linux (Make)

### 1) Install dependencies

You need:

- `gcc`
- `make`
- `pkg-config`
- GTK 3 development files
- GStreamer 1.0 development files
- libcurl development files
- json-c development files

Example package names on Debian/Ubuntu:

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libgstreamer1.0-dev libcurl4-openssl-dev libjson-c-dev
```

### 2) Configure

```bash
./configure
```

Optional install prefix:

```bash
./configure --prefix=/usr/local
```

### 3) Build

```bash
make
```

### 4) Run

```bash
./radio_app
```

### 5) Install (optional)

```bash
sudo make install
```

## Basic controls

- **Search Directory**: query stations by name or category/tag.
- **Play**: start playback for the selected station.
- **Stop**: stop playback.
- **Download Stream**: begin writing the current stream to a local file.
- **Stop Download**: stop recording and close the output file.

## Roadmap

- Add station favorites and recent history.
- Add configurable recording/output directory.
- Improve metadata display (bitrate/codec/country).
- Add basic tests and CI build checks.
- Add keyboard shortcuts and accessibility polish.
