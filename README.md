# Steam Frame camera capture

Tools for reading the Steam Frame's cameras and eye tracking on the headset itself
(Linux ARM64, SteamVR 2.18.1) while VR keeps running. Buffer layouts were worked out
on one unit; they are not a Valve API and may break with SteamVR or firmware updates.

| Tool | Output | Needs |
| --- | --- | --- |
| [`frametap`](#frametap-outward-cameras) | 6 outward cameras: Arcturus color pair (2464×2464 NV12), SLAM pair (1056×1024 gray), upper pair (640×480 gray) | `sudo`, debugfs |
| [`eyetap`](#eyetap-eye-cameras) | 2 eye cameras, 400×400 IR, 90 fps | `sudo` |
| [`eyegaze`](#eyegaze-gaze) | Gaze rays and head-relative yaw/pitch (CSV) | OpenVR SDK headers |
| `unpack_raw10` | Offline MIPI RAW10 → 16-bit converter; not used by the tools above | — |

Arcturus is the attached third-party color camera module.

## Build

```sh
make                                          # all tools
make tools/frametap tools/eyetap tools/unpack_raw10   # without the OpenVR SDK
```

Requires C and C++17 compilers, Linux UAPI headers, and make. `eyegaze` needs the OpenVR
SDK headers in `../openvr` and links the installed runtime loader
(`/opt/steamvr/bin/linuxarm64/libopenvr_api.so`) via rpath; override with
`make OPENVR_SDK=... STEAMVR_LIBDIR=...`. FFmpeg is only used for previews.

## Common behavior

- `frametap` and `eyetap` need `sudo` to duplicate buffer handles from another process
  (`pidfd_getfd`; handle duplication, not a ptrace attach). After that they drop to the
  sudo caller's UID/GID; a direct root login keeps root and warns.
- Output goes to a new 0700 directory (default under `captures/`); files are 0600. The
  output directory must not exist. `.part` files are incomplete; each frame's JSON
  record is written last and marks it complete.
- No tool opens a camera device, writes shared memory, takes locks, or starts/stops
  services.
- `captures/` and compiled binaries are Git-ignored. Captures can contain room
  contents, screens, faces, serial numbers, and calibration data.

## frametap: outward cameras

```sh
sudo ./tools/frametap --list
sudo ./tools/frametap --count 2
sudo ./tools/frametap --camera arcturus-a --count 4 --output captures/my-capture
sudo ./tools/frametap --count 2 --full --output captures/my-full-capture
sudo ./tools/frametap --pid PID --list        # when several XRService processes exist
```

Cameras: `all` (default), `arcturus-a`, `arcturus-b`, `slam-a`, `slam-b`, `upper-a`,
`upper-b`. The headset must be awake with passthrough active. After 5 s without new
frames the tool exits instead of saving stale ones.

### Output

| Camera | Image | File | Stride | Image buffer | Metadata buffer |
| --- | --- | --- | ---: | ---: | ---: |
| Arcturus | 2464×2464 color | `.nv12` | 3328 | 12,353,536 B | 1,064,960 B |
| SLAM | 1056×1024 mono | `.pgm` | 1056 | 1,769,472 B | 151,552 B |
| Upper | 640×480 mono | `.pgm` | 640 | 462,848 B | 8,192 B |

Arcturus files keep the row padding: Y is 3328×2464, then interleaved UV (same stride,
1232 rows) at byte 8,200,192; 12,300,288 bytes total. Decode at the padded width and crop:

```sh
ffmpeg -f rawvideo -pixel_format nv12 -video_size 3328x2464 -i arcturus-a-000001.nv12 \
  -vf 'crop=2464:2464:0:0' -frames:v 1 arcturus-a-000001.png
ffmpeg -i slam-a-000001.pgm -frames:v 1 slam-a-000001.png
```

Images are unprocessed: no normalization, undistortion, or rotation; color matrix and
range are uncalibrated. Each JSON record holds geometry, stride, source PID/fds/inodes,
buffer sizes, the first metadata u64 (as a string; units unknown), and the host copy
time. `--full` also saves the whole image and metadata buffers (`.image.bin`,
`.metadata.bin`), including undecoded tails.

Tracking cameras alternate scene exposures with short controller-LED exposures, so some
valid frames are nearly black. Capture several frames to get a scene view.

### How it works

1. Find the single `XRService` process (or `--pid`) and pin it with `pidfd_open`.
2. From `/sys/kernel/debug/dma_buf/bufinfo`, take known-size `udmabuf` objects attached
   to `acb8000.isp`; match their inodes in XRService's `/proc/PID/fdinfo`. Size alone is
   not enough; display buffers share sizes.
3. Require 32 image and 32 metadata buffers per camera kind (two 16-slot rings), sorted
   by inode with image/metadata alternating; split into blocks `a` and `b`.
4. Duplicate with `pidfd_getfd`, re-check inode and size, map `PROT_READ`, and bracket
   reads with `DMA_BUF_IOCTL_SYNC(READ)`.
5. Per camera, pick the slot with the second-largest metadata value (the newest may
   still be written), copy it, and discard the copy if the value changed.

### Limits

- Image/metadata pairing is inferred from allocation order; it is not a stable ABI.
- DMA sync gives cache coherency, not a producer fence. Torn frames are reduced, not
  ruled out. Cameras are copied one after another, not as synchronized stereo pairs.
- Metadata units, frame timing, and VR latency impact are not measured.

### Camera identity

From the media topology and XRService logs:

| XRService name | Sensor | Subdevice | Video node |
| --- | --- | --- | --- |
| passthrough_left | arcimx616, I2C 0-001a | `/dev/v4l-subdev29` | `/dev/video0` |
| passthrough_right | arcimx616, I2C 0-0010 | `/dev/v4l-subdev28` | `/dev/video3` |
| slam_left | og01a1b | `/dev/v4l-subdev30` | `/dev/video13` |
| slam_right | og01a1b | `/dev/v4l-subdev31` | `/dev/video9` |
| upper_left | og0ve10 | `/dev/v4l-subdev32` | `/dev/video6` |
| upper_right | og0ve10 | `/dev/v4l-subdev33` | `/dev/video7` |

XRService set the nodes up in the order video9, video13, video6, video7, video3, video0,
so block `a` is probably right for Arcturus/SLAM and left for upper (inferred; not
confirmed with a covered lens).

The Arcturus sensor pad reports `Y10_1X10`, but XRService configures NV12 and the
buffers decode as processed color. Sensor Bayer data is not reachable this way.

## eyetap: eye cameras

```sh
sudo ./tools/eyetap --list                   # ring slots and timestamps
sudo ./tools/eyetap --count 90               # 1 s of consecutive frames per eye
sudo ./tools/eyetap --eye a --count 30 --output captures/right-eye
```

Camera `a` is the right eye, `b` the left (verified; filenames keep `a`/`b` because
the mapping comes from ring order). The headset must be worn: eye tracking stops
off-head, and the tool exits after 3 s without new frames.

Each frame is `eye-{a,b}-NNNNNN.pgm` (400×400 8-bit IR, sensor orientation, unprocessed)
plus a JSON record with slot, frame timestamp, host copy time, buffer inode, and
ring-header values. Every new frame is saved once, in order. Camera `b` images appear
rotated/flipped relative to `a`.

### Where the frames are

The eye cameras run through the ADSP (`CStereoAdspCams`); no eye-camera V4L2 node is
used. Frames land in a 16 MiB `udmabuf` held by the `eyetracking` process and also mapped
by `dsp_service`, vrserver, XRService, and vrcompositor.

| Item | Value |
| --- | --- |
| Ring | 8 slots from `0x234000`, pitch `0x40000`; slots 0–3 camera `a`, 4–7 camera `b` |
| Image start, slot k | `0x234000 + k·0x40000 + 256 + 64·k`, plus 64 for `k ≥ 4` |
| Image | 400×400 bytes, row stride 512 |
| Timestamp | u64 LE nanoseconds at image start − 64 |
| Ring header (at `0x234000`) | per camera, 16 bytes apart: exposure (s), gain, frame rate (inferred) |

The image start was derived two ways (padding position and last non-zero byte) and they
agree. Timestamps are 11.11 ms apart (90 fps) and run ~0.48 s ahead of host
`CLOCK_MONOTONIC`; their clock is unknown. The two cameras' frames are 33–82 µs apart.

### How it works

Find the `eyetracking` process, require exactly one 16 MiB udmabuf, duplicate it with
`pidfd_getfd`, and map it `PROT_READ`. Per camera, take the second-newest timestamp,
copy the 400 rows, and discard the copy if the timestamp changed. Unlike `frametap`,
there is no `DMA_BUF_IOCTL_SYNC`: CPU processes also write this buffer, and a cache
operation on it is not known to be side-effect free. The tool refuses to run if the
buffer or timestamps don't match the expected layout.

### Limits

- Offsets are hard-coded from observation.
- No producer fence: a rewrite that keeps the same timestamp would go undetected
  (`tear_free_guaranteed` is `false`).
- Other ring-header fields and the timestamp clock are undecoded.

## eyegaze: gaze

```sh
./tools/eyegaze --seconds 20 --output captures/gaze/run.csv   # no sudo
./tools/eyegaze --seconds 5 --interval-ms 20                   # CSV on stdout
```

Needs SteamVR, the normal `eyetracking` service, and the headset worn. Runs as the
desktop user (refuses root). Exit codes: 0 usable gaze, 2 no usable gaze (e.g. off-head),
1 error.

### How it works

- Connects as an OpenVR background app and checks `Prop_SupportsXrEyeGazeInteraction_Bool`.
- Action manifest [`tools/eyegaze_actions.json`](tools/eyegaze_actions.json) declares
  `/actions/gaze/in/eye_gaze` of type `eyetracking` (per Valve's
  [Steam Frame input doc](https://partner.steamgames.com/doc/steamhardware/steamframe/input)).
- Binding [`tools/eyegaze_bindings_frame_hmd.json`](tools/eyegaze_bindings_frame_hmd.json)
  maps it for `frame_hmd`:
  `"eyetracking": [{"output": "/actions/gaze/in/eye_gaze", "path": "/user/head/eyetracking"}]`.
  Valve doesn't document this; the format came from vrserver validation strings and the
  path from the driver's `/eyetracking` component. Without it SteamVR reports "no
  configured binding" and all samples are inactive. Keep both JSON files beside the binary.
- Each poll calls `UpdateActionState`, `GetEyeTrackingDataRelativeToNow` (standing
  universe, 0 s), and the HMD pose. `ForNextFrame` needs `WaitGetPoses`, which a
  background app must not call.

CSV columns: `host_monotonic_ns, input_error, active, valid, tracked, usable,
hmd_activity, head_pose_valid, origin_{x,y,z}_m, target_{x,y,z}_m, head_dir_{x,y,z},
head_yaw_deg, head_pitch_deg`. Head frame: +x right, +y up, −z forward; yaw positive
right, pitch positive up. Unusable samples leave the ray fields blank.

### Limits

- `target` is a fixation point (ray lengths 0.11–59.9 m); use the direction, not the
  depth.
- Values change on every poll because the API evaluates at call time (inferred). The
  service requests 90 fps and the eye cameras run at 90 fps; the gaze estimator's own
  rate is unmeasured.
- Accuracy is unmeasured (no ground-truth targets). Straight-ahead read 3–8° left and
  8–17° down.
- Gaze and head pose come from two API calls, not one atomic sample.

## Verification (2026-09-29, this unit)

**frametap**
- `--list` found 96 image/metadata pairs across six cameras.
- In standby, capture exited after 5 s without saving.
- `--count 2 --full` saved 12 frames ([`captures/passive-live/`](captures/passive-live/));
  each camera's frames advanced and differed, sizes matched the table, and all six views
  decoded ([contact sheet](captures/passive-live/scene-contact-sheet.png): Arcturus, SLAM,
  upper rows; `a` left, `b` right; not simultaneous).
- `upper-b --count 8` with default naming worked; invalid counts, selectors, PIDs, and an
  existing output directory were rejected with the old capture unchanged.
- vrserver, XRService, and vrcompositor kept their PIDs throughout.

**eyetap**
- Live runs before and after the code cleanup: [`captures/eyes-live/`](captures/eyes-live/)
  (`--count 30`) and [`captures/eyes-retest/`](captures/eyes-retest/) (`--count 90`). Every
  frame complete (400×400, 0600, no `.part` files), all distinct, gaps 11.08–11.13 ms
  (no drops), camera pairs 33–82 µs apart. The retest caught a blink
  ([contact sheet](captures/eyes-retest/contact-sheet.png)).
- Left eye closed ([`captures/eyes-left-closed/`](captures/eyes-left-closed/)): camera
  `b` showed a closed lid in every sampled frame, camera `a` an open eye.
- Headset off: `--list` read the ring; capture exited after 3 s without saving.
- Passthrough pauses logged during this work matched playspace setting changes
  (`SessionSettingsChanged`), not the tool.

**eyegaze**
- Standby: binding loaded, all samples inactive, exit 2.
- Directed sweep, 25 s at 10 ms ([`captures/gaze/head-relative-sweep.csv`](captures/gaze/head-relative-sweep.csv)):
  2,500/2,500 samples usable, head still (origin moved < 6 mm).

  | Look | Time | Yaw | Pitch |
  | --- | --- | ---: | ---: |
  | Straight | 0–4 s | −3° to −8° | −8° to −17° |
  | Left | 4.5–9 s | −31.5° | −9.5° |
  | Right | 10.5–17.5 s | +31° | −2° |
  | Up | 18–23.5 s | −0.5° | +28° |
  | Down | 24.5 s (end) | +6° | −37° |

## What didn't work

- **OpenVR `IVRTrackedCamera_006`**: `HasCamera` true, but `GetCameraFrameSize` failed
  for all frame types, even after `AcquireVideoStreamingService`. Releasing the service
  coincided with passthrough dropping; **Menu → camera off → on** restored it. Don't run
  acquire/release probes during use.
- **`/dev/video99`**: a SteamVR `v4l2loopback` device (1920×1080 RGB24); it returned a
  black frame.
- **XRService snapshot** (`--snapshotCamerasAndExit`): produces processed PNGs of all six
  cameras plus controller exposures and exposure/gain metadata
  ([`captures/2026-09-29_xrservice-snapshot/`](captures/2026-09-29_xrservice-snapshot/)),
  but needs exclusive camera access, i.e. VR stopped:

  ```sh
  # Stops VR. Run from /opt/steamvr/drivers/cv/bin/linuxarm64.
  systemctl --user mask --runtime steamvr.service && systemctl --user stop steamvr.service
  ./XRService --documentsRoot /tmp/xrsnap-docs --snapshotCamerasAndExit --showLogToConsole
  # Restore:
  systemctl --user unmask --runtime steamvr.service
  systemctl --user start gamescope-session.target steamvr.service steamvr-v4l2cam.service
  ```

  Without the mask, SteamVR restarts and races for the cameras. Don't kill XRService
  alone; `driver_cv` restarts it.
- **Direct V4L2 capture** (Arcturus `Y10P` stride 3088, SLAM `GREY`, via `v4l2-ctl`
  with SteamVR stopped): returned structured noise; upper cameras failed with
  EPIPE/timeout. XRService restarted mid-test and tracking fell back to 3DoF. An strace
  of the working snapshot ([`captures/bringup-trace/`](captures/bringup-trace/)) shows why:
  XRService programs the arcimx616 through `VIDIOC_S_CTRL(V4L2_CID_BRIGHTNESS)` used as a
  register write (address `value & 0xffff`, data `(value >> 16) & 0xff`: timing,
  crop/binning, stream-on `0x0100`), plus SPI, media-graph, and DSP/ICP setup. A
  plain format change doesn't reproduce that.
- **`unpack_raw10`** is left from that attempt. It unpacks RAW10 (4 pixels in 5 bytes)
  to 16-bit LE, shifted left 6 (`--no-shift` keeps 0–1023). It rejects malformed or
  overflowing geometry before opening files and requires distinct input and output
  files (including via links); an existing output file is overwritten.
- **Reopening another process's DMA-buf via `/proc/PID/fd/N`**: ENXIO/EACCES.
  `pidfd_getfd` works.
- **Not pursued**: tracking/dataset recording (`StartTrackingRecording`; may include
  screen and audio; OCC output with h264/h265/jpeg codecs), the private
  `IVRCameraPassthroughInternal_001` source switch used by
  [frame-passthrough-shortcuts](https://github.com/KominoVR/frame-passthrough-shortcuts)
  (settings, not pixels), and `XRPCClient` in `libArcturusPerception.so` (no safe second
  client found).
- **`/dev/shm/eye-server.mmap`** (324,122 bytes, shared by `eyetracking` and
  `driver_cv`, guarded by process-shared mutexes): only the first 4 KiB held data (gaze
  and control, inferred); the rest was zero. Not an image path. Read once with
  `read(2)`; locks never taken.
- **Not tried**: `eyetracking --calib <seconds>` (saves PNGs but opens the cameras
  itself, conflicting with the running service), `--logGazes` (gaze logs only; needs a
  restart), the 32 MiB DSP buffer (small changing regions, undecoded), and
  `GetEyeTrackedFoveationCenter` (foveation points, not a gaze ray).

## Open questions

- Arcturus Bayer (pre-ISP) data while VR runs.
- Left/right mapping of the outward-camera `a`/`b` blocks.
- Frame metadata: `frametap` marker units, eye timestamp clock, remaining ring-header
  fields.
- Producer-fenced (guaranteed untorn) capture.
- Gaze accuracy against known targets.
