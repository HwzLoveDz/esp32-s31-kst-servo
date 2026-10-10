# FUI source preview

This builds **the same `main/fui.c` used on the Korvo**, with LVGL 9.5's software
renderer and a deterministic host fixture. No web imitation, servo, CAN adapter,
background service or native Windows executable is used. The executable exits
after screenshots and action tests.

`00-offline.png` is the initial, unread state. All populated screens visibly say
**PREVIEW FIXTURE / SYNTHETIC INPUT / NOT HARDWARE DATA**. They demonstrate layout
and widget wiring, not device measurements or hardware acceptance.

The USB fixtures additionally cover a connected adapter without the required
ONE_SHOT capability (`00b-usb-capability-fault.png`) and USB ready without any
servo response (`00c-usb-no-servo.png`). LINK shows USB identity and setup status;
LOG explicitly labels USB TX acceptance rather than CAN transmission success.

On Linux / WSL, using the existing LVGL 9.5.0 source directory:

```sh
cmake -S preview -B "$HOME/.cache/kst-fui-build" \
  -DLVGL_SOURCE_DIR=/absolute/path/to/lvgl-9.5.0 \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build "$HOME/.cache/kst-fui-build" --parallel 8
"$HOME/.cache/kst-fui-build/kst_fui_preview" preview/screens
python3 preview/convert.py preview/screens
```

The renderer writes 800 x 480 PPM files; `convert.py` uses the Python standard
library to losslessly encode those buffers as PNG. The source renderer enables assertions
and checks all application action IDs, values, both ARM states, presets and
fine position changes, and cancellation / confirmation of CENTER and SAVE.
It also checks that a +/-45 degree preset is clamped to a tighter +/-20 degree
host envelope even if device limit readback still reports +/-100 degrees.
The `10-extended-flags-read-only.png` fixture uses the observed 3.66 feature
layout (`0x5C90`) to verify that both REV and ALARM write buttons are disabled
with a visible reason; returning to a known layout restores the controls.
`11-outside-limit-recovery.png` injects -100.5 degree feedback: the actual value
remains visible, sweep is disabled, and SELECT NEAREST LIMIT only edits the
target. Explicit SEND is checked separately at -100 and +100 degrees.
`12-limit-restore-unverified.png` verifies the diagnostic restore fault locks
motion and parameter writes, including SAVE, while readback and PAUSE remain
available. Neither fixture demonstrates physical motion or 360 degree support.
`13a-axis-positive.png`, `13b-axis-zero.png`, and `13c-axis-negative.png`
show +45 / 0 / -45 degrees in the mounted-horn view: left-up / up / right-up.
The mirrored shaft drawing and tick labels do not invert numeric feedback,
the history chart, or any motion target.
It does not replace driver tests or physical touchscreen / CAN validation.

UI font requirements: Montserrat 12, 14, 16, 20, 24, 36, 48. Native widgets used:
label, button, slider, spinbox, dropdown and chart. `fui.c` has no ESP-IDF or OS
dependencies and uses only LVGL, standard C and the controller's `app_model.h`.

## Screen behavior

* MONITOR: actual angle, a mechanically themed shaft diagram driven by fresh
  angle readback, current, temperature, feedback ages, limits and angle history.
  Missing measurements display `--`; stale values remain explicitly marked and
  the dynamic shaft disappears. The 200 ms chart samples only readback values;
  missing data inserts a gap. The stale threshold comes from the controller
  snapshot (800 ms by default). READ limits and the conservative HOST envelope
  are shown separately; position presets and sliders obey the HOST envelope.
* MOTION: target edit, presets and 0.1 degree steps, ARM, explicit SEND, sweep
  amplitude / cycle and PAUSE TEST. Editing a target never moves the shaft.
  Pause stops command production; it is not a motor torque-disable function.
* SERVICE: scroll for every documented readback, flags, limits, report controls,
  center adjustment and persistence. SAVE and CENTER both require confirmation.
  Readback and proposed fields are distinct. Firmware update is explicitly
  outside the supplied protocol, while version query is available.
* LINK: local connection parameters separate from device node / bitrate writes;
  SAVE / power-cycle / reconnect requirement and pending parameters are visible.
* LOG: last transactions, counters, latched alerts and CAN bus recovery.

Touch targets are at least 40 pixels in each dimension. Slider's visual track
is 12 pixels high with an 18-pixel expanded hit area and a 40-pixel knob.
The Korvo still needs real touch calibration, cable / transceiver and load testing.
