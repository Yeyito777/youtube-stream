# youtube-stream

Simple YouTube Live streaming command for this workstation.

`youtube-stream`:

1. requires a title, description, and thumbnail path before streaming,
2. updates title/description/thumbnail through the YouTube Data API, then opens the public stream page,
3. captures the X11 primary monitor with `gpu-screen-recorder` using GPU encoding,
4. mixes microphone + system audio with a boosted microphone gain,
5. draws the same purple `#a855f7` click-through outline around the captured monitor style used by `active-development/record`,
6. pushes FLV/RTMP to YouTube until you press `Ctrl+C` in the launching terminal.

## Install

```sh
make
make install
```

This installs:

- `~/.local/bin/youtube-stream`
- `~/.local/lib/youtube-stream/youtube-stream-api`
- `~/.local/lib/youtube-stream/youtube-stream-outline`

Make sure `~/.local/bin` is on your `PATH`.

## First-time YouTube setup

You need both:

- a YouTube Live stream key in `~/.config/youtube-stream/config`, and
- OAuth credentials for the YouTube account/channel in:

```text
~/.config/youtube-stream/client_secret.json
~/.config/youtube-stream/oauth-token.json
```

Run:

```sh
youtube-stream --setup
```

If YouTube asks to verify a phone number or request live-streaming access, complete that first; YouTube may take up to 24 hours to enable live streaming.

After streaming access is enabled, copy your YouTube stream key from the Stream tab and paste it when `youtube-stream` prompts. It stores the key in:

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

Instead of `YOUTUBE_STREAM_KEY`, you may set a full `YOUTUBE_STREAM_URL`.

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
the current non-complete Live broadcast, or create/bind a new one to your default
stream key if the previous broadcast has already completed. It then sets the
title, description, optional privacy, and thumbnail, and opens the public watch
page in vimbrowser unless `--no-browser` is passed.

Press `Ctrl+C` in the launching terminal to stop the stream.

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
| `--dry-run` | Print the gpu-screen-recorder/ffmpeg pipeline without updating YouTube metadata or starting the stream. |
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
YOUTUBE_STREAM_FPS=30
YOUTUBE_STREAM_VIDEO_CODEC=h264
YOUTUBE_STREAM_VIDEO_BITRATE_KBPS=6800
YOUTUBE_STREAM_ENCODER=gpu
YOUTUBE_STREAM_FALLBACK_CPU_ENCODING=yes
YOUTUBE_STREAM_MIC_SOURCE=@DEFAULT_SOURCE@
YOUTUBE_STREAM_SYSTEM_SOURCE=@DEFAULT_MONITOR@
YOUTUBE_STREAM_MIC_GAIN=10.5
YOUTUBE_STREAM_SYSTEM_GAIN=0.675
YOUTUBE_STREAM_AUDIO_BITRATE=192k
```

The mic gain is intentionally boosted so voice is louder relative to desktop audio:

```text
mic gain:    10.5
system gain: 0.675
```

## Notes

- `gpu-screen-recorder` does the screen capture and video encoding. `ffmpeg` receives the already encoded H.264 video, captures/mixes PulseAudio/PipeWire audio live, AAC-encodes the mixed audio, and muxes/pushes RTMP.
- If the previous broadcast has completed, the API helper creates a fresh Live broadcast and binds it to your existing/default stream key before starting the encoder push.
- YouTube may still require the Studio stream to have auto-start enabled, or you may need to click **Go live** after the stream preview appears. This tool updates title/description/thumbnail metadata through the YouTube Data API, opens the public watch page, and starts the encoder push; it does not force-click destructive YouTube Studio actions by default.

## Dependencies

- `gpu-screen-recorder`
- `ffmpeg`
- `xrandr`
- `vimbrowser-cli`
- X11 development libraries to build the outline helper (`libX11`, `libXext`)
