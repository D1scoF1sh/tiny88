# Tiny 2.1/88

An integer port of [WollyMonjac's Tiny 2.1](https://github.com/WollyMonjac/Tiny-2.1) for an IBM PC 5150 with a 4.77 MHz 8088. It runs under DOS and supports CGA, MDA, and a dual-screen Neural Monitor.

I used Codex for much of the 8088 port, with Claude also contributing to the initial port, and tested it on my own IBM PC 5150.

## Run under DOS

Copy `TINY88.EXE`, `TINY88.BIN`, and `TINY0.BIN` from [dos](dos) into the same directory on the PC. Copy `TINYPFX.BIN` there too if you want the optional cache for common prompt beginnings. Tiny88 needs about 520 KB of free conventional memory.

```text
C:\>CD \TINY88
C:\TINY88>TINY88.EXE
```

See the [DOS instructions](dos/README.TXT) for controls and display modes.

## Build from source

The inference engine and 8088 assembly are in [source](source). From the repository root on Linux, run:

```sh
WATCOM=/path/to/open-watcom ./tools/build.sh
```

The script uses Open Watcom v2 and writes `tools/build/TINY88.EXE`. Use that executable with the same model files described above. See [HOWTO.txt](tools/HOWTO.txt) to regenerate the model files or cache from upstream inputs.

The upstream code's MIT notice is in [LICENSE.TXT](LICENSE.TXT). See [DATA_LICENSES.md](DATA_LICENSES.md) for the provenance and data notices for the bundled model and cache.
