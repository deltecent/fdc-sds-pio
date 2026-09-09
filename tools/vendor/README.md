# Vendored flashing dependencies

`tools/flasher.py` bundles everything it needs to erase and reflash the ESP32 over
USB, so an end user needs only **Python 3** — no `pip install`, no toolchain, no
network. These are the third-party packages it imports, copied here verbatim and put
on `sys.path` by the flasher.

| Package | Version | Purpose | License |
| --- | --- | --- | --- |
| [`esptool`](https://github.com/espressif/esptool) | 4.5.1 | the actual flash protocol (erase / write) | GPL-2.0 (`licenses/esptool-LICENSE-GPLv2.txt`) |
| [`pyserial`](https://github.com/pyserial/pyserial) (`serial/`) | 3.5 | serial port I/O + port enumeration | BSD-3-Clause (`licenses/pyserial-LICENSE.txt`) |
| [`reedsolo.py`](https://github.com/tomerfiliba/reedsolomon) | 1.6.0 | Reed–Solomon codec esptool imports for image handling | Public domain (`licenses/reedsolo-LICENSE.txt`) |

Only the flashing path is included. esptool's `espefuse`/`espsecure` tools — and their
heavier dependencies (`cryptography`, `ecdsa`, `bitstring`) — are **not** bundled; they
are used only for secure-boot / eFuse work this project never does. The flash path
(`erase_flash`, `write_flash`) needs only `pyserial` + `reedsolo`.

`esptool` is GPL-2.0. `tools/flasher.py` invokes it as a **subprocess**
(`python -m esptool …`), never imports it, so it stays an aggregated standalone program
rather than a linked library.

## Refreshing

To move to a newer esptool (or refresh the deps), from the repo root:

```
rm -rf tools/vendor/esptool tools/vendor/serial tools/vendor/reedsolo.py
python3 -m pip install --no-deps --target tools/vendor \
    esptool==<ver> pyserial==<ver> reedsolo==<ver>
# then trim what flashing doesn't use:
rm -rf tools/vendor/bin tools/vendor/espefuse tools/vendor/espsecure \
       tools/vendor/*.dist-info tools/vendor/__pycache__
```

Keep the `licenses/` files in sync with whatever versions you land on. After a refresh,
confirm it still reaches the serial layer without the trimmed deps:

```
PYTHONPATH=tools/vendor python3 -m esptool --chip esp32 --port /dev/nonexistent \
    write_flash 0x0 ota/merged.bin        # expect "Could not open ... port", not ImportError
```
