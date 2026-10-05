# Graphics & desktop — framebuffer, LVGL, EquiX/ThorVG

Equinox OS draws directly to a VESA linear framebuffer — there is no
X server and no compositor daemon. Every program draws into its own
canvas; the kernel owns the screen.

## Framebuffer

- Mode: **1360×768×32** via VBE/VESA (`boot/start.asm` asks for it;
  `kernel.cpp` forces it again after `vesa_init_from_multiboot()`),
  with a VGA text fallback.
- The kernel composites: text console + per-task pixel windows + mouse
  cursor.

## Multi-console text mode

Each task has its own cell buffer and cursor. F1/F2 switches the
visible console; background tasks can `printf` freely without
corrupting the foreground screen. A background task's first pixel draw
snapshots the text console; on exit the text console is re-rendered.

## Per-task draw windows

Ring-3 programs get a clipped view of the screen:

- `SYS_SETCLIP` — kernel intersects the requested rectangle with the
  screen and stores it in the task; every subsequent pixel syscall is
  clipped.
- `SYS_DRAWLINE` — Bresenham line, focus-gated.
- `put_pixel`/`fill_rect` are the workhorses behind mtcc's graphics
  builtins (`fb_info`, `put_pixel`, `fill_rect`, `draw_line`,
  `set_clip`, `pollkey`, `mouse_state`).

Games (DOOM, Libgame ports) lean on these plus the audio syscalls
(`spk_tone`, `snd_beep`).

## LVGL 9 apps

`kernel/library/UI/` ships LVGL 9 applications — file manager,
settings editor, text editor — rendered over the framebuffer with PS/2
mouse input. Portal: the `gui` shell builtin launches the desktop.

## EquiX desktop (ThorVG)

`kernel/gui/equinox_desktop/` is the native vector-styled desktop,
rendered with the bundled **ThorVG** (`kernel/thorvg/`, `tvgdemo`,
`tvgbench`, `tvginfo` for probing). Assets and per-frame caches live
in the dynamic GUI arena (starts at `0x3400000`), sized from the GRUB
memory map.

## Mouse & input

PS/2 mouse on IRQ12 (`drivers/ps2_mouse.cpp`), IntelliMouse wheel
extension when negotiated, events surfaced as `mouse_state`/
`key_event` syscalls. Absolute cursor is clamped to the framebuffer.

## Rendering a screenshot patch

No display protocol exists; for docs and regression you script the
framebuffer: `scripts/fat32_demo_shot.py`, `debug_screen.py` and
`probe_screen.py` dump or assert on pixels from QEMU.

## Performance notes

- The framebuffer is identity-mapped; drawing is `uint32` stores.
- Per-task clip windows make a "window" cheap — no per-window
  compositing pass.
- `tvgbench` measures ThorVG throughput on the guest; use it before
  blaming the renderer.
