# youtube-stream

Simple YouTube Live stream manager for this workstation.

This is a C project. The stream manager, YouTube API helper, and X11 outline
helper all live under `src/` and are built by `make`; there is no Bash
orchestrator in the hot path.

`youtube-stream`:

1. requires a title, description, and thumbnail path before streaming,
2. creates/reuses a 1080p60 YouTube Live Stream resource, updates title/description/thumbnail through the YouTube Data API, then opens the public stream page,
3. captures the X11 primary monitor with `gpu-screen-recorder`'s KMS/GPU path and streams H.264 directly to YouTube at 1080p60/12 Mbps,
4. captures microphone + system audio into one AAC stream,
5. draws the same purple `#a855f7` click-through outline around the captured monitor style used by `active-development/record`,
6. opens a whale-themed stream-manager TUI with nvim-style controls for the audio gain sent to stream,
7. pushes FLV/RTMP directly to YouTube until you press `q`/`Ctrl+C` in the launching terminal.

## Install

```sh
make check
make install
```

This installs:

- `~/.local/bin/youtube-stream`
- `~/.local/lib/youtube-stream/youtube-stream-api`
- `~/.local/lib/youtube-stream/youtube-stream-outline`

Make sure `~/.local/bin` is on your `PATH`.

Source layout:

```text
src/youtube-stream.c          main stream manager / TUI / process orchestration
src/youtube-stream-api.c      YouTube Data API helper using libcurl + json-c
src/youtube-stream-outline.c  X11 capture outline helper
```

## First-time YouTube setup

You need OAuth credentials for the YouTube account/channel in:

```text
~/.config/youtube-stream/client_secret.json
~/.config/youtube-stream/oauth-token.json
```

With OAuth configured, the helper creates or reuses a YouTube Live Stream key
resource whose CDN profile is `1080p` + `60fps`, then passes that ingest URL to
the selected encoder. A manually configured `YOUTUBE_STREAM_KEY`/`YOUTUBE_STREAM_URL`
is only needed as a fallback, for dry-runs, or if you disable API-created stream keys.

Run:

```sh
youtube-stream --setup
```

If YouTube asks to verify a phone number or request live-streaming access, complete that first; YouTube may take up to 24 hours to enable live streaming.

After streaming access is enabled, `youtube-stream --setup` can still store a
fallback YouTube stream key from the Stream tab in:

```text
~/.config/youtube-stream/config
```

with mode `0600`.

You can also create the config manually:

```sh
mkdir -p ~/.config/youtube-stream
chmod 700 ~/.config/youtube-stream
cat > ~/.config/youtube-stream/config <<'EOF'
YOUTUBE_STREAM_KEY=xxxx-xxxx-xxxx-xxxx-xxxx
YOUTUBE_STREAM_CHANNEL_ID=UCxxxxxxxxxxxxxxxxxxxxxxxx
YOUTUBE_STREAM_TITLE='My stream title'
YOUTUBE_STREAM_DESCRIPTION='My stream description'
YOUTUBE_STREAM_THUMBNAIL=/path/to/thumbnail.png
# Optional; omit to leave current broadcast privacy unchanged.
YOUTUBE_STREAM_PRIVACY=public
EOF
chmod 600 ~/.config/youtube-stream/config
```

Instead of `YOUTUBE_STREAM_KEY`, you may set a full `YOUTUBE_STREAM_URL`. Normal
live runs prefer the YouTube Data API ingest URL from the matching 1080p60 Live
Stream resource.

Optional metadata:

```sh
# These can be provided in the config, but per-stream CLI args are usually better:
YOUTUBE_STREAM_TITLE='My stream title'
YOUTUBE_STREAM_DESCRIPTION='My stream description'
YOUTUBE_STREAM_THUMBNAIL=/path/to/thumbnail.png
# Optional; omit to leave current broadcast privacy unchanged.
YOUTUBE_STREAM_PRIVACY=public

# Optional override if vimbrowser-cli is not on PATH. vimbrowser is now only
# used to open the public watch page, not to edit Studio metadata.
VIMBROWSER_CLI=/path/to/vimbrowser-cli
```

## Usage

Minimum invocation:

```sh
youtube-stream \
  -t 'My stream title' \
  -d 'My stream description' \
  -p /path/to/thumbnail.png
```

Before starting the encoder, `youtube-stream` uses the YouTube Data API to find
or create a reusable 1080p60 Live Stream resource, find the current non-complete
Live broadcast or create/bind a fresh one, and return the stream ingest URL for
the selected encoder. It then sets the title, description, optional privacy, and
thumbnail, and opens the public watch page in vimbrowser unless `--no-browser`
is passed.

Press `q` in the stream-manager TUI, or `Ctrl+C` in the launching terminal, to
stop the stream. On shutdown, the tool stops the local encoder and asks the
YouTube Data API to transition the active broadcast to `complete`.

Interactive terminals open the stream-manager TUI by default. It hides raw
encoder output, shows stream/watch/status/capture info, and writes encoder logs
to:

```text
~/.local/state/youtube-stream/current-encoder.log
```

TUI keys use nvim-style movement:

| Key | Action |
| --- | --- |
| `j` / `k` | Select mic/system gain row. |
| `h` / `l` | Decrease/increase selected gain by 5 percentage points. |
| `H` / `L` | Decrease/increase selected gain by 25 percentage points. |
| `0` | Mute selected stream gain. |
| `=` | Reset selected stream gain to 100%. |
| `r` | Reapply stream gains and refresh YouTube status/viewer info. |
| `q` | Stop the stream cleanly. |

### Flags

| Flag | Meaning |
| --- | --- |
| `-t TITLE`, `--title TITLE` | Required stream title. Can also be set with `YOUTUBE_STREAM_TITLE`. |
| `-d TEXT`, `--description TEXT` | Required stream description. Can also be set with `YOUTUBE_STREAM_DESCRIPTION`. |
| `-p PATH`, `--thumbnail PATH` | Required PNG/JPEG thumbnail path. Can also be set with `YOUTUBE_STREAM_THUMBNAIL`. |
| `-v STATUS`, `--privacy STATUS` | Optional privacy: `public`, `unlisted`, or `private`. If omitted, the current broadcast privacy is left unchanged. Can also be set with `YOUTUBE_STREAM_PRIVACY`. |
| `--setup` | Configure/prompt for the YouTube RTMP stream key, then exit. |
| `--no-browser` | Do not open the public watch page in vimbrowser. Metadata is still updated through the API. |
| `--no-outline` | Do not draw the purple capture outline. |
| `--no-audio` | Stream video only; disables microphone/system audio capture. |
| `--no-tui` | Disable the stream-manager TUI and print logs directly. |
| `--dry-run` | Print the encoder pipeline without updating YouTube metadata or starting the stream. |
| `-h`, `--help` | Show command help. |

### Examples

```sh
# Public stream with explicit metadata.
youtube-stream -t 'My stream title' -d 'My description' -p /path/to/thumb.png -v public

# Long option equivalents.
youtube-stream --title 'My stream title' --description 'My description' --thumbnail /path/to/thumb.png --privacy public

# Leave privacy unchanged, but skip the visual outline.
youtube-stream --no-outline -t 'My stream title' -d 'My description' -p /path/to/thumb.png

# Video-only stream.
youtube-stream --no-audio -t 'My stream title' -d 'My description' -p /path/to/thumb.png -v unlisted

# Inspect the command pipeline without changing YouTube or starting a stream.
youtube-stream --dry-run --no-browser -t test -d 'desc' -p /path/to/thumb.png -v private

# The stream key must be the YouTube Live stream key from Live Control Room,
# not a Google API key. API keys usually start with AIza and will be rejected.
```

## Configuration

See `config.example` for all supported values.

Important defaults:

```sh
YOUTUBE_STREAM_FPS=60
YOUTUBE_STREAM_VIDEO_CODEC=h264
YOUTUBE_STREAM_VIDEO_BITRATE_KBPS=12000
YOUTUBE_STREAM_CAPTURE_BACKEND=gsr-direct-rtmp
YOUTUBE_STREAM_CDN_RESOLUTION=1080p
YOUTUBE_STREAM_CDN_FRAME_RATE=60fps
YOUTUBE_STREAM_CREATE_LIVE_STREAM=yes
# gpu-screen-recorder direct RTMP settings:
YOUTUBE_STREAM_GSR_TUNE=quality
YOUTUBE_STREAM_GPU_PERF=high
YOUTUBE_STREAM_ENCODER=gpu
YOUTUBE_STREAM_FALLBACK_CPU_ENCODING=no
YOUTUBE_STREAM_TUI=1
YOUTUBE_STREAM_TUI_STATUS_INTERVAL=15
YOUTUBE_STREAM_MIC_SOURCE=default_input
YOUTUBE_STREAM_SYSTEM_SOURCE=default_output
YOUTUBE_STREAM_MIC_GAIN=10.5
YOUTUBE_STREAM_SYSTEM_GAIN=0.675
YOUTUBE_STREAM_AUDIO_BITRATE=192k
```

The stream manager starts `gpu-screen-recorder` with both default system output
and default mic input in one AAC stream. The TUI then adjusts only the source
output volumes that feed the stream:

```text
mic gain:    10.5
system gain: 0.675
```

## Notes

- The default capture path is `gpu-screen-recorder` direct RTMP: KMS/GPU capture + GPU H.264 encode + FLV/RTMP output to YouTube in one process at 1080p60, CFR, and 12 Mbps CBR. This avoids the `ffmpeg x11grab` CPU-copy bottleneck and also avoids the older GSR→pipe→ffmpeg remux path.
- The TUI gain controls adjust the PipeWire/PulseAudio source-output volumes for the stream capture (`gsr-default_output` and `gsr-default_input`). This changes what is sent to stream without changing your global desktop or mic volume.
- The API helper defaults to `YOUTUBE_STREAM_CDN_RESOLUTION=1080p` and `YOUTUBE_STREAM_CDN_FRAME_RATE=60fps`, creates/reuses a matching Live Stream resource (`YOUTUBE_STREAM_CREATE_LIVE_STREAM=yes`), and returns that ingest URL to the wrapper. This matters: YouTube will not reliably expose an `hd1080`/60fps watch-page rendition if the bound Live Stream resource is only configured for a lower CDN profile.
- The C rewrite intentionally supports the known-good `gsr-direct-rtmp` backend only. The previous ffmpeg/x11grab and GSR pipe experiments were removed from the active orchestrator because the direct GSR RTMP path is the one that held 1080p60 smoothly on this workstation.
- The wrapper can best-effort pin the AMD GPU performance level to `high` during capture, then restores the previous level on shutdown.
- If the previous broadcast has completed, the API helper creates a fresh Live broadcast and binds it to the matching 1080p60 Live Stream resource before starting the encoder push.
- On normal exit or `Ctrl+C`, the wrapper attempts to mark the active YouTube broadcast `complete` through the API so shutdown is deterministic instead of relying only on YouTube auto-stop.
- YouTube may still require the Studio stream to have auto-start enabled, or you may need to click **Go live** after the stream preview appears. This tool updates title/description/thumbnail metadata through the YouTube Data API, opens the public watch page, and starts the encoder push; it does not force-click destructive YouTube Studio actions by default.

## Dependencies

- C build toolchain (`cc`, `make`, `pkg-config`)
- development libraries: `json-c`, `libcurl`, `libX11`, `libXext`
- `xrandr`
- `vimbrowser-cli` (optional; used to open the watch page)
- `gpu-screen-recorder`
- `pactl` (for TUI live audio gain controls)
