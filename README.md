# Tiny 2.1/88

An integer port of [WollyMonjac's Tiny 2.1](https://github.com/WollyMonjac/Tiny-2.1) for an IBM PC 5150 with a 4.77 MHz 8088. It runs under DOS and supports CGA, MDA, and a dual-screen Neural Monitor.

Copy the contents of [dos](dos) to one directory on the PC and run `TINY88.EXE`. See [the DOS instructions](dos/README.TXT) for controls, display modes, and the optional prompt cache.

The engine is in [source](source). To rebuild on Linux with Open Watcom v2:

```sh
WATCOM=/path/to/open-watcom ./tools/build.sh
```

See [HOWTO.txt](tools/HOWTO.txt) for the model conversion and prompt-cache tools. The upstream MIT notice is in [LICENSE.TXT](LICENSE.TXT).
