# Steam Frame camera access

Local investigation of this Linux ARM64 headset, with owner-authorized captures.
Arcturus is the attached third-party color-camera module, as identified by the owner.
This is an experimental, firmware-specific interface, not a Valve-supported API.

## Working method: passive DMA-buffer capture

`tools/frametap.c` copies existing camera buffers from the running XRService.
It does not open `/dev/video*`, activate OpenVR camera streaming, change sensor
registers, attach a debugger, or stop/restart any service.

```sh
make
sudo ./tools/frametap --list
sudo ./tools/frametap --count 2
```

Keep the headset awake and passthrough active. The tool does not wake it.
Default output: a new `captures/tap-YYYYmmdd-HHMMSS-PID/` directory. Other options:

```sh
# Parent directory must exist; the output directory must NOT already exist.
sudo ./tools/frametap --camera arcturus-a --count 4 --output captures/my-capture
sudo ./tools/frametap --camera upper-b --count 8
sudo ./tools/frametap --count 2 --full --output captures/my-full-capture
# Optional: select a particular running XRService when more than one exists.
sudo ./tools/frametap --pid PID --list
```

Camera selectors: `all`, `arcturus-a`, `arcturus-b`, `slam-a`, `slam-b`,
`upper-a`, `upper-b`. `all` selects available supported pairs; absent pairs are
reported. Partial or unexpected ring layouts are rejected, not guessed.

Prerequisites:

- Linux with `pidfd_open`, `pidfd_getfd`, udmabuf, DMA-buffer sync, and an already
  mounted debugfs exposing `/sys/kernel/debug/dma_buf/bufinfo`.
- Running XRService with its normal sensor configuration and advancing frames.
- Root access via `sudo`, for debugfs and the ptrace permission check on
  `pidfd_getfd`. This is handle duplication, not a ptrace attach. No Yama setting
  or persistent executable capability is changed. CAP_SYS_PTRACE alone would
  not necessarily grant debugfs access.
- A C compiler, Linux UAPI headers, and make. FFmpeg is only needed for previews.

After discovery and descriptor duplication, the program drops supplementary
groups and root UID/GID to the sudo caller before creating output. Captures are
0600, new capture directories 0700. A direct root invocation without sudo identity
keeps root privileges and warns. Images and original traces can contain private
room contents, screens, serials, and calibration data; do not publish them blindly.

`captures/` and the compiled `tools/frametap` / `tools/unpack_raw10` binaries are
Git-ignored. Capture links below refer to local investigation artifacts, not files
distributed with the repository. Keep custom output paths under `captures/` to
retain that protection; ignoring files does not remove already tracked content.

### Output and decoding

Each successful frame has an image and a JSON completion record with format,
geometry, stride, source PID/fds/inodes, allocation sizes, the first metadata
uint64, and the host-monotonic copy interval. The metadata uint64 is stored as a
string to avoid losing precision in JSON consumers. Its units remain unknown.

| Camera kind | Active image | File | Native stride | Image allocation | Metadata allocation |
| --- | --- | --- | ---: | ---: | ---: |
| Arcturus | 2464 × 2464 color | `.nv12` | 3328 | 12,353,536 B | 1,064,960 B |
| SLAM | 1056 × 1024 mono | `.pgm` | 1056 | 1,769,472 B | 151,552 B |
| Upper tracking | 640 × 480 mono | `.pgm` | 640 | 462,848 B | 8,192 B |

Arcturus files retain native row padding: Y occupies 3328 × 2464 bytes, followed
by interleaved UV with the same stride and 1232 rows. UV starts at byte 8,200,192;
the saved NV12 payload is 12,300,288 bytes. Decode at the padded width, then crop:

```sh
ffmpeg -f rawvideo -pixel_format nv12 -video_size 3328x2464 \
  -i captures/my-capture/arcturus-a-000001.nv12 \
  -vf 'crop=2464:2464:0:0' -frames:v 1 captures/my-capture/arcturus-a-000001.png

ffmpeg -i captures/my-capture/slam-a-000001.pgm \
  -frames:v 1 captures/my-capture/slam-a-000001.png
```

These commands illustrate filenames; select a directory containing that camera.
The mono PGM payloads are exactly 1,081,344 and 307,200 bytes, respectively,
plus their PGM headers. No normalization, undistortion, rotation, or exposure
filtering is applied. Color-range/matrix interpretation has not been calibrated;
FFmpeg's preview is not a colorimetric reference.

`--full` additionally saves `.image.bin` and `.metadata.bin` containing the whole
two allocations, including padding and unknown tails. Extra mono-buffer data is
not decoded; calling it an ICP output or UV plane would be an inference.
`.part` files are incomplete. A matching published JSON file is the completion
record; interrupted/error runs may leave partial files or already completed frames.

Tracking streams include scene-exposure and short controller-exposure images.
Some perfectly readable frames therefore look nearly black. The tool does not
decode exposure classes or discard dark frames. Capture several frames when you
need a scene view rather than a controller-LED exposure.

### Discovery, pairing, and consistency limits

1. Locate exactly one process whose executable basename is `XRService`, or check
   the explicit PID. Pin it with `pidfd_open` and detect process exit.
2. Parse DMA debugfs for known-size `udmabuf` objects attached to `acb8000.isp`.
   Cross-reference their inodes against XRService's `/proc/PID/fdinfo`. Size alone
   is insufficient: other display/processing buffers share sizes.
3. Expect 32 distinct images and 32 metadata buffers per available kind: two
   16-slot rings. Sort by inode, not reusable fd number. Require image/metadata
   allocations to alternate, and split each kind into two blocks of 16.
4. Duplicate only selected handles using `pidfd_getfd`; recheck exporter, inode,
   and size on the duplicates. Map with `PROT_READ`. Bracket reads with
   `DMA_BUF_IOCTL_SYNC(START|READ)` and `END|READ`.
5. Sample the first little-endian metadata uint64. Skip the largest observed
   value and select the second-largest distinct value. Require advancement beyond
   startup and beyond the preceding saved frame; recheck the selected marker
   before and after copying. Discard a copy when that marker changes.
6. Fail after five seconds without an advancing, stable candidate. Standby
   buffers are not silently presented as new frames. A stopped XRService also
   aborts the run. Restart/hotplug requires fresh discovery.

**These are heuristics, not a completed-buffer ownership protocol.** Inode order
matched the observed allocations, but is not a stable kernel/firmware camera ABI.
The archived strace abbreviates QBUF's plane descriptors, so it does not directly
prove image/metadata association. The alternating-order check rejects some changes,
not every possible wrong association.

DMA-buffer sync provides CPU cache coherency, not a producer fence or exclusion
against ISP/ICP writers. Second-newest selection plus a marker recheck reduces
races but cannot prove a coherent exposure or detect every torn frame. Copies of
different cameras are sequential, not synchronized stereo acquisitions. No
sub-microsecond timing claim, exact frame-rate claim, or conversion to nanoseconds
is established for the opaque metadata value. Host copy times are not exposure
times. Sustained-rate performance and VR latency impact have not been benchmarked.

### Camera identity

Media topology and XRService logs identify these sensor routes on this device:

| XRService name | Sensor | Sensor subdevice | Capture node |
| --- | --- | --- | --- |
| passthrough_left | arcimx616, I2C 0-001a | `/dev/v4l-subdev29` | `/dev/video0` |
| passthrough_right | arcimx616, I2C 0-0010 | `/dev/v4l-subdev28` | `/dev/video3` |
| slam_left | og01a1b | `/dev/v4l-subdev30` | `/dev/video13` |
| slam_right | og01a1b | `/dev/v4l-subdev31` | `/dev/video9` |
| upper_left | og0ve10 | `/dev/v4l-subdev32` | `/dev/video6` |
| upper_right | og0ve10 | `/dev/v4l-subdev33` | `/dev/video7` |

The recorded setup order was video9, video13, video6, video7, video3, video0.
**[INFERENCE]** allocation block `a` is consequently right for Arcturus/SLAM and
left for upper tracking; block `b` is the opposite. The tool intentionally uses
`a`/`b`, not anatomical labels. A covered-lens experiment would be needed to
independently confirm each association. Neither mapping nor buffer sizes should
be assumed to survive a firmware update or different startup/hotplug ordering.

The pad format `Y10_1X10` on arcimx616 does not mean the accessible DMA allocation
contains RAW10. XRService configured NV12, and the captured buffers decode as
color NV12. This establishes processed color access before application rendering,
not sensor Bayer access. Whether another live pipeline exposes Bayer data is
unresolved. Stopping VR alone did NOT yield a verified Bayer capture.

## Verification and retained captures

- `make` compiled `frametap` with `-Wall -Wextra -Wpedantic` without diagnostics.
- `--list` enumerated 96 image/metadata pairs across all six cameras.
- A standby capture exited with a five-second no-advancement error, saving no stale
  image. XRService logged user absence and paused tracking cameras.
- After the owner woke the headset, `--count 2 --full` saved 12 frames under
  [`captures/passive-live/`](captures/passive-live/). Every camera had advancing
  markers and different pixel hashes between its two frames. Image sizes matched
  the layouts above, normal payloads matched full-buffer prefixes, and output
  ownership/modes were UID 1000 / 0600.
- All six scene views decoded and were visually inspected. The
  [scene contact sheet](captures/passive-live/scene-contact-sheet.png) has Arcturus
  in row 1, SLAM in row 2, upper in row 3; `a` left, `b` right. It uses Arcturus
  frame 2 and mono frame 1, without brightening. These are not simultaneous frames.
- An explicit-PID, single-camera `upper-b --count 8` capture also succeeded,
  exercising default output naming and capture without `--full`.
- Invalid counts, unknown selectors, and a non-XRService PID were rejected.
- Reusing an existing output directory was rejected, and its original capture
  hash stayed unchanged. All eight single-camera completion records had advancing
  markers and correctly sized mono payloads.
- The observed VR process PIDs remained vrserver 13509, XRService 13623,
  vrcompositor 13629 across passive capture. No service-control commands were
  issued for the passive tests. This is not a headset-render latency measurement.

These captures were made during the 2026-09-29 investigation. Original processed
snapshot images are separately retained in
[`captures/2026-09-29_xrservice-snapshot/`](captures/2026-09-29_xrservice-snapshot/).

## Other methods investigated

### OpenVR public tracked camera: unsuccessful here

A background OpenVR client queried `IVRTrackedCamera_006`. `HasCamera` returned
present, but `GetCameraFrameSize` failed for all three frame types, including after
`AcquireVideoStreamingService`. Acquisition success did not imply usable frames.

Releasing the service/shutting down the probe coincided with compositor logging
`Depth mesh block queues disconnected successfully`; the owner reported losing
the VR camera view. Exact causality was not proved. The owner restored it via
**Menu → camera off → camera on**. Do not repeat acquire/release probes during use.
The unsupported probe is not part of the delivered tool.

### SteamVR virtual camera: not physical-camera access

`/dev/video99` is a `v4l2loopback` device labelled SteamVR, reported as 1920×1080
RGB24 at 30 fps. FFmpeg captured a black frame. Installed `v4l2cam` strings refer
to mirroring an overlay; this is not evidence of a sensor-frame export route.

### XRService snapshot: working, but requires exclusive camera ownership

With the normal runtime held down and camera ownership released, this invocation
completed in about 11 seconds during the earlier experiment:

```sh
# Historical exclusive-mode command, NOT safe alongside the running XRService.
# Run from /opt/steamvr/drivers/cv/bin/linuxarm64 with its normal library setup.
./XRService --documentsRoot /tmp/xrsnap-docs --snapshotCamerasAndExit --showLogToConsole
```

It produced RGB PNGs for both Arcturus cameras, grayscale PNGs for all four
tracking cameras plus their controller-exposure variants, `cameraMetadata.json`,
and `logs.tar.gz`. This is processed imagery, not Bayer RAW10. The original
snapshot metadata contains exposure/gain and sensor identifiers.

The earlier experiment used a temporary `systemctl --user mask --runtime
steamvr.service` to prevent competing restarts; this also interrupts the VR session.
The mask was removed, SteamVR/gamescope restored, and the owner confirmed VR was
back. Recovery commands used in that investigation were:

```sh
systemctl --user unmask --runtime steamvr.service
systemctl --user start gamescope-session.target steamvr.service steamvr-v4l2cam.service
```

They are recorded for incident recovery, not part of passive capture. Do not kill
XRService alone: driver_cv can restart it. Do not run another capture owner until
`fuser` confirms the relevant devices are free, and account for automatic restarts.

### Direct V4L2 RAW10/GREY capture: failed experiment

Stopping `steamvr.service` without preventing automatic restart raced against
XRService. The direct experiment set Arcturus to 2464×2464 `Y10P` with stride
3088 and SLAM to 1056×1024 `GREY`, using `v4l2-ctl` streaming. It obtained bytes
from Arcturus and SLAM, but rendered images were structured noise, not valid scene
captures. Upper-camera attempts failed with EPIPE/timeout. XRService restarted
while a camera was still held, encountered busy devices, and the owner reported
fallback to 3DoF. The later restored runtime recovered normal operation.

`tools/unpack_raw10.c` is the retained offline MIPI RAW10 unpacker: each group of
four pixels uses four high-byte values and a fifth byte holding two low bits per
pixel. Its default little-endian 16-bit output is left-shifted by six; `--no-shift`
retains values 0–1023. Correct unpacking did not repair the misconfigured capture.
It is **not** the decoder for the working NV12 tap.
Dimensions must be positive decimal integers; the unpacker rejects malformed or
overflowing geometry before opening files. Input and output must be different
files, including when reached through hard links or symlinks. An existing,
distinct output file is overwritten.

A successful standalone snapshot was traced with `strace -f -tt -v -s 200` over
`openat,ioctl,write,pwrite64,execve`. Evidence is retained in
[`captures/bringup-trace/`](captures/bringup-trace/): raw trace, console output, and
a decoded operation sequence. The trace showed `VIDIOC_S_CTRL` on arcimx616 using
`V4L2_CID_BRIGHTNESS` as a register-write transport:

- Register address: `value & 0xffff`.
- Register byte: `(value >> 16) & 0xff`.
- Writes included sensor timing, crop/binning, and stream-on register `0x0100`.
- SPI, media topology, DSP/ICP and other initialization were also involved.

Thus setting a video-node format alone did not reproduce XRService's configured
pipeline. Blindly replaying register writes is not a supported access method.
Existing CSID2 errors were observed before the failed experiment as well; those
messages alone do not establish its cause. Compression/DSC in Arcturus was a
hypothesis, not a demonstrated explanation for the failed capture.

### Dataset recording and private camera configuration: leads, not verified exports

Installed dashboard code exposes Room and Tracking recording via
`VRHTML.VRSystem.StartTrackingRecording` / `StopTrackingRecording`; driver strings
include `start_tracking_recording` / `stop_tracking_recording`. The current settings
had `allowTrackingCameraRecording`, `allowTrackingScreenRecording`, and
`allowTrackingAudioRecording` enabled. This can involve screen/audio, not only
cameras; privacy scope matters. No OCC dataset export or decoder was tested here.
XRService help advertised recording codecs `h264`, `h265`, `jpeg`, and OCC tooling
symbols were present. This is not proof that an OCC file contains lossless Bayer.
`vrcmd --startcapture/--stopcapture` refers to an SVL network capture, not this route.
A working command-line camera-recording sequence has not been established.

The owner's linked [frame-passthrough-shortcuts project](https://github.com/KominoVR/frame-passthrough-shortcuts)
uses `IVRCameraPassthroughInternal_001` slots 9/10 to read/write a five-byte camera
configuration, preserving unrelated options. Its
[source](https://github.com/KominoVR/frame-passthrough-shortcuts/blob/main/src/openvr_camera_source.cpp)
is a source-selection lead, not a pixel-buffer API. It was inspected, not executed.
Private `XRPCClient` exports in `libArcturusPerception.so` were also investigated;
no safe second-client frame-export protocol was established.

Directly reopening another process's DMA-buf through `/proc/PID/fd/N` failed
(ENXIO/EACCES). `pidfd_getfd` with authorized privilege succeeded instead; that is
the mechanism used by `frametap`, not reading arbitrary process memory.

## Eye tracking: recorded entry points, not yet captured

Eye-tracking access is separate from these six outward-facing cameras.

- Installed executable: `/opt/steamvr/tools/eyetracking/bin/linuxarm64/eyetracking`.
  Historical logs in `~/.local/share/Steam/logs/eyetracking.txt` include
  `CGazeEstimatorCdsp`, `Failed to grab cdsp input buffer`, and user-presence
  start/stop messages. They establish a DSP-backed path, not a tested raw-eye export.
- `/dev/shm/eye-server.mmap` exists. Its binary layout, synchronization, and ownership
  protocol have not been decoded; do not guess offsets or write into it.
- Local SDK: `../openvr/headers/openvr.h`, `IVRInput` methods
  `GetEyeTrackingDataRelativeToNow` and `GetEyeTrackingDataForNextFrame` return
  `VREyeTrackingData_t` for an eye-tracking action handle and tracking origin.
  The struct has `bActive`, `bValid`, `bTracked`, `vGazeOrigin`, and `vGazeTarget`.
  These are gaze results, not eye-camera pixel buffers. Action setup/binding and
  validity must be exercised before claiming usable gaze data. SDK declarations
  alone do not prove the installed runtime implements the same interface version.
- Installed binary strings include legacy OVM6211/ADSP-style camera paths;
  historical logs contain an `exp5v rail ... rework is required` shutdown warning.
  Neither proves the current eye-tracking path is broken or that those legacy
  device nodes are usable.

No eye-camera frames or application gaze samples were captured in this work.

## Remaining technical limits

Verified deliverable: passive full-resolution NV12 Arcturus images and native
8-bit mono images from the four tracking cameras, with repeatable capture commands.
Unresolved: Arcturus sensor Bayer access, definitive anatomical/ring association,
producer-fenced coherent stereo capture, metadata clock/exposure decoding, and eye
camera/gaze capture. These require additional interface evidence, not another
format guess or a disruptive restart hidden inside the capture tool.
