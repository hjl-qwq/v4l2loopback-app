# v4l2loopback-app

Application to verify v4l2loopback on Allwinner T536 (Linux5.10.198), including write and capture.

## Usage

### v4l2_write - Video Frame Generator

**Purpose:** Generates and streams synthetic video frames to a V4L2 loopback device.

**Key Features:**

- Creates alternating video patterns: 75% standard color bars and solid color frames
- Supports multiple solid colors: Red, Green, Blue, White, and Black
- Configurable frame rate (default: 15 FPS for embedded system stability)
- Uses V4L2 memory-mapped buffers for efficient video output
- Implements precise frame timing control
- Automatically cycles between patterns (2 seconds color bars, 2 seconds solid colors)

**Technical Details:**

- Resolution: 640×480 pixels
- Pixel Format: YUYV (YUV422)
- Buffer Strategy: Double-buffered MMAP
- Device: `/dev/video1` (configurable in code)
- Includes driver compatibility fixes for older kernels

**Usage:**

`./v4l2_write`

The application runs continuously until terminated, creating a video source that can be captured by other applications.

### v4l2_capture - Video Frame Capturer

**Purpose:** Captures video frames from a V4L2 device and saves them to a file.

**Key Features:**

- Captures video in YUYV format from specified V4L2 device
- Saves raw video data to a file for analysis or playback
- Configurable device path and output file
- Includes timeout handling and buffer management
- Signal handling for graceful shutdown (Ctrl+C)
- Tracks frame count and write statistics

**Technical Details:**

- Resolution: 640×480 pixels (must match write application)
- Pixel Format: YUYV (YUV422)
- Frame Rate: 15 FPS (synchronized with write application)
- Buffer Strategy: Double-buffered MMAP
- Default Output: `yuyv_cycle_fix.yuv`

**Usage:**

`./v4l2_capture [-d /dev/videoX] [-o output_file.yuv]`

Options:

- `-d`: Specify V4L2 device (default: `/dev/video1`)
- `-o`: Specify output file (default: `yuyv_cycle_fix.yuv`)
