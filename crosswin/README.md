# CrossWin Stage 2–7A prototype

This directory adds the constrained TCP prototype on top of the Stage 1
Geometry Oracle in `../geometry`.  It currently provides a single remote
output, one `WS_POPUP` proxy window, scale 1, TCP, real Weston
`xdg_toplevel + wl_shm` export, Linux-owned drag geometry, and persistent
BGRA8888 framebuffers with incremental damage.  GPU/dmabuf, scene subtrees,
multi-window policy and a high-performance transport remain later stages.

## Wire contract

CWNP v2 has a fixed 24-byte header, explicitly serialized in little endian:

| Byte offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | magic (`CWNP`) |
| 4 | 2 | protocol version (2) |
| 6 | 2 | message type |
| 8 | 4 | flags |
| 12 | 4 | payload length |
| 16 | 8 | wire sequence |

`common/protocol.c` never sends a native struct.  Its incremental `CwDecoder`
handles arbitrary TCP fragmentation/coalescing and rejects bad magic/version,
unknown mandatory messages, payloads over 64 MiB, invalid frame stride/size,
and malformed presentation or pointer payloads.

`WINDOW_FRAME` has the content version (`frame_sequence`) and is the initial
full frame, fallback, and resynchronization mechanism. `WINDOW_DAMAGE` is an
atomic collection of tightly-packed BGRA surface-local rectangles; its
`base_frame_sequence` must equal the receiver's cached frame before the patch
is applied. A mismatch causes `WINDOW_FRAME_REQUEST`, and Linux replies with a
full `WINDOW_FRAME`. `WINDOW_RESIZE` replaces the persistent content buffer;
it is followed by a full frame. Frame content versions and presentation
versions are deliberately independent.

`WINDOW_PRESENT` is atomic: it contains surface-local source rect,
remote-output-local destination rect, visibility, and a presentation sequence.
For visible v1 presentation, source and destination dimensions must match, so
rendering is always 1:1.  The Windows Agent ACKs only after applying that state.

Pointer events carry this presentation sequence.  The fake server stores the
last 64 presentation states and maps `client -> surface` with
`presented_fragment_to_surface_local()` from Stage 1.  A stale sequence is
dropped rather than reinterpreted using the newest presentation.

## Components

- `common/`: explicit wire codec, decoder, and BGRA pixel declaration.
- `fake-server/`: deterministic Linux test pattern, TCP server, scripted Stage
  2 presentations, 64-entry presentation history, click crosshair, and a
  Linux-owned fake move grab.
- `windows-agent/`: C++17 Win32/Winsock/GDI implementation.  `WSAAsyncSelect`
  places socket and HWND events on one UI thread.  It uses `WS_POPUP`, a
  top-down `StretchDIBits` DIB, signed `GET_X_LPARAM`, `ScreenToClient` for
  wheels, `TrackMouseEvent`, and mouse capture.
- `tests/`: deterministic parser, session, scripted presentation TCP, pointer
  round-trip TCP, and Stage 7A damage/resync/CRC tests.

详细的 Stage 7A 编译、Windows 视觉验证、静态拖动回归及本地自动失步恢复步骤见
`../docs/stage7a-damage-test.txt`。

## Linux checks

```text
make test
make integration
make input-integration
make sanitize
```

`integration` verifies the full Stage 2 sequence, including an ACK before
every next presentation.  `input-integration` clicks client `(133,211)` on
presentation `src=[220,0 580x600]`, verifies the resulting crosshair at
surface `(353,211)`, then exercises a Linux-owned 100-pixel fake drag.

## Windows build and visual check

On Windows, double-click `build-windows-agent.cmd`.  It locates a Visual Studio
x64 C++ toolchain, builds with `/W4 /WX`, and writes
`build/crosswin-agent.exe`.  From a Visual Studio x64 Native Tools prompt, you
can alternatively run `make windows-build-help` for the concrete commands.
Then start the server and agent in separate terminals:

```text
./build/fake-server --listen 0.0.0.0 --port 44600 --script-stage2 --trace-present
crosswin-agent.exe --host <linux-ip> --port 44600 --trace-protocol --trace-present --trace-input
```

The proxy must show the deterministic BGRA grid without vertical flip or B/R
swap.  In scripted presentation 3 its first proxy pixel must correspond to
surface x=220, and the HWND must be `[0,300 580x600]`.  In `--interactive`
mode, clicking `client=(133,211)` draws a 21x21 yellow crosshair under that
same click; dragging changes position only after a Linux `WINDOW_PRESENT`.
