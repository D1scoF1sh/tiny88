# Model and prompt cache attributions

`dos/TINY88.BIN` and `dos/TINY0.BIN` are generated from WollyMonjac's Tiny 2.1 trained model (`dos/TINY.BIN`) with [tools/convert.py](tools/convert.py). The optional `dos/TINYPFX.BIN` stores precomputed model state for question prefixes selected from the upstream training question generator and [tools/extra.txt](tools/extra.txt). The [build notes](tools/HOWTO.txt) describe how to regenerate these files.

Upstream identifies the following data in the Tiny 2.1 training facts and trained network. The attribution applies here because Tiny88 bundles converted model files and a cache generated using the upstream model and question generator. [Upstream Tiny 2.1 data licenses](https://github.com/WollyMonjac/Tiny-2.1/blob/main/DATA_LICENSES.md)

| Source | Upstream files | Data license |
| --- | --- | --- |
| [mledoze/countries](https://github.com/mledoze/countries) | `training/facts/geo2.tsv`, `training/facts/geo3.tsv` | [ODbL 1.0](https://opendatacommons.org/licenses/odbl/1-0/) |
| [Bowserinator/Periodic-Table-JSON](https://github.com/Bowserinator/Periodic-Table-JSON) | `training/facts/elements.tsv` | [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/) |

Upstream identifies its remaining fact files as MIT-licensed. The upstream code notice is in [LICENSE.TXT](LICENSE.TXT).
