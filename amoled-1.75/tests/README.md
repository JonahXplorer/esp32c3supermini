# Touch, full-frame display and audio capture regression tests

From the repository root on Windows with MSVC and the Windows SDK installed:

```powershell
& .\amoled-1.75\tests\run-host-tests.ps1
```

The tests compile the production `components/sugarat_board/touch.c` and
`main/screen_port.c` directly, replacing only their GPIO/I2C/LVGL dependencies.
They exercise the reference attribute-read order, interrupt event handling,
15-byte packet length, read acknowledgment (including empty/invalid packets),
finger release, failures and interrupt arrival during a read. Full-frame
display updates are compared against an inverse clockwise-rotation
oracle, then every one of the 466×466 native touch positions is checked against
the pixel shown at that position.

The native AMOLED path checks tiled clockwise rotation, including the last two
rows/columns of the 466-pixel frame. Both logical buffers must remain unchanged;
the UI can draw the next frame while the worker owns the submitted buffer.
The completion semaphore is consumed after rotation, before LVGL may reuse that
buffer during SPI. DIRECT rectangle batching and single-pixel corner updates
must preserve every other pixel in the complete physical frame. A full
frame uses 15 windows with a 32-row, 29824-byte internal DMA staging buffer.
Timer mocks check 3300/700 microsecond UI deadlines without tick rounding.

`qspi_window_test.c` compiles the shared production QSPI adapter. It verifies
ordered CASET/RASET/pixel queueing, driver-owned descriptors and pixel copies,
timeout recovery without freeing in-flight storage, partial queue failure,
returned DMA-error descriptors, and the existing command API.

Outputs stay in the existing ignored `build/` directory. These tests do not open
a serial port or replace on-device verification of touch orientation and audio.

`audio_capture_test.c` compiles the production `audio.c` with deterministic DMA,
AEC and time substitutes. It checks selection of FD_LOW_COST, budget expiry and
blocking ticks, preservation of the 320/192-sample output split across a yield,
and sample continuity during speaker/volume changes. It also covers partial I2S
reads, reference-slot alignment, read-error recovery, and over-budget counters.
The substitutes verify control flow and buffering, not acoustic quality or real
device CPU load, watchdog behavior or DMA overflow rates.
