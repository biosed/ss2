# SUE/PCK reference slice

The native `ss2_sprite` tool reads one exact archive entry and one selected frame on request. It emits top-down, little-endian 16-bit pixel words to stdout and reports the original signed origin and dimensions on stderr. It links zlib and has no graphics-device dependency.

```sh
./build/ss2_sprite "$HOME/ss2-private/game/app/menu_common.sue" scrollbar_sp.pck 1 > "$HOME/ss2-private/frame-1.le16"
```

## Retail comparison

On the M1 Pro, the Gold 2.2 build 13718 menu archive (SHA-256 `1afe6a07dbbb0d62ffd521e99e9302e6a289eeff5a0a1b97cef91d9b193cf2d9`) yielded three frames for scrollbar_sp.pck. A separate Python reader inflated the retail archive's tables and entry using zlib. The resulting 668-byte PCK had SHA-256 `107af790dc28d249f3e083b2352edca031a09a8f26945291e2e3a693dbc43895`.

The pixel oracle was the unchanged Image.cxx translation unit from pinned WarToolKit. A private harness provided file I/O and an in-memory 16-bit surface for DirectDraw lock/unlock. For each frame, `cmp` compared all 220 native output bytes with the oracle. All three matched. This comparison checks CPU decoding. Retail-game compositing, transparency rules, and color presentation remain unverified. [Reference decoder](https://github.com/americusmaximus/WarToolKit/blob/8c5b925db477b310bb26f65d66f4b1cab8de0990/Source/pckView/Image.cxx)

| Frame | Origin | Size | SHA-256 of pixel words |
| --- | --- | --- | --- |
| 0 | 1, 0 | 11 × 10 | d97a0f58baab480a14a2c4ab46f5de3f266991813a721a23f5f406ab2faa8ab1 |
| 1 | 1, 10 | 11 × 10 | 73f23f4bbee052e6743d54c792acd7451c16d33ea10e61732ae078860a3929f8 |
| 2 | 1, 20 | 11 × 10 | 5a64dec051999b2534aec6426360df559adcccbf07c990f9ca95eeee66e8fdab |

A second comparison exercised a retail entry spanning 28 compressed chunks. Frame zero of win_jap.pck produced an 800 × 600 image at origin (0, 0). All 960,000 bytes matched the same independent decoder; the pixel SHA-256 was `2abb50dbba53b9fba9442fe613f49a271ef650d6a6005c5744b8a25af82be12d`. The separately inflated PCK SHA-256 was `af7e44ca6d719806c4d2ff2698f390b5ca7d6cfde59a45f22b7ae99104338ff0`. The private retail unsue.exe comparison was attempted under a reference-only Wine/FEX setup; it exited before producing output. The archive oracle for these runs was the separate Python reader.

## Supported subset

The SUE reader follows the FZFS header's relative index offset, inflates three descriptor tables, and supports inline raw entries (type 2) and independently zlib-compressed chunks (type 8). External file references are unsupported. Lookup uses the exact stored name. The reader caps each uncompressed index table at 16 MiB, and the selected entry and decoded pixel buffer at 64 MiB each. These are tool limits.

Frame decoding currently accepts tag 0xA5 and rows whose literal and repeated 16-bit runs account for the declared width. Other tags and sparse or variant encodings require their own evidence. The synthetic CTest exercises both storage types, multi-chunk reads, multiple frames, negative origins, literal/repeat runs, invalid indices, malformed data and expansion limits. All public fixtures are generated in a temporary directory.
