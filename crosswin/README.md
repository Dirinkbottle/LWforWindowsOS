# CrossWin Stage 2–3 prototype

This directory adds the constrained TCP prototype on top of the Stage 1
Geometry Oracle in `../geometry`.  It is intentionally limited to one remote
output, one `WS_POPUP` proxy window, scale 1, a full top-down BGRA8888 frame,
and TCP.  It contains no Wayland, GPU renderer, shared-memory transport, or
multi-window policy.

## Wire contract

CWNP v1 has a fixed 24-byte header, explicitly serialized in little endian:

| Byte offset | Size | Field |
| --- | ---: | --- |
| 0 | 4 | magic (`CWNP`) |
| 4 | 2 | protocol version (1) |
| 6 | 2 | message type |
| 8 | 4 | flags |
| 12 | 4 | payload length |
| 16 | 8 | wire sequence |

`common/protocol.c` never sends a native struct.  Its incremental `CwDecoder`
handles arbitrary TCP fragmentation/coalescing and rejects bad magic/version,
unknown mandatory messages, payloads over 64 MiB, invalid frame stride/size,
and malformed presentation or pointer payloads.

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
- `tests/`: deterministic parser, session, scripted presentation TCP, and
  pointer round-trip TCP tests.

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
