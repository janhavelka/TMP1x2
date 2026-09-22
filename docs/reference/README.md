# TI TMP1x2 reference archive

Retrieved 2026-09-22. The PDFs are the specification; the notes below are an implementation aid. `manifest.json` records original URLs, document revisions, immutable Git revisions where available, sizes, and SHA-256 digests. `SHA256SUMS` also covers the research inventories and this document.

## Specifications

| Local document | TI revision | Purpose |
| --- | --- | --- |
| [tmp102.pdf](tmp102.pdf) | SBOS397I, June 2024 | Catalog TMP102 register interface and limits |
| [tmp112.pdf](tmp112.pdf) | SBOS473L, July 2024 | Catalog TMP112 A/B/D/N and X2SON variants |
| [tmp102-q1.pdf](tmp102-q1.pdf) | SBOS702E, September 2021 | Automotive TMP102; slower conversion timing |
| [tmp112-q1.pdf](tmp112-q1.pdf) | SLOS887H, August 2026 | Automotive TMP112 and TMP112D differences |
| [sbaa588a.pdf](sbaa588a.pdf) | SBAA588A, January 2025 | TI fixed-point decoding application note |

The `.txt` files are local `pypdf` extractions for searching; PDF tables and diagrams remain authoritative. Later packaging addenda can have newer dates than the actual electrical-specification revision.

## Register and command map

All register words travel **most significant byte first**. Register reads should send a one-byte pointer then perform a two-byte read, preferably with repeated START. A register write is three bytes: pointer, MSB, LSB. Pointer bits 7:2 must be zero. Registers do not form a documented auto-increment burst interface: read each register separately.

| Pointer | Register | Access | Power-up state |
| --- | --- | --- | --- |
| `0x00` | Temperature | Read | 0 C until first conversion completes |
| `0x01` | Configuration | Mixed read/write | `0x60A0`; OS and AL subsequently reflect live state |
| `0x02` | Low threshold | Read/write | 75 C (`0x4B00`, normal format) |
| `0x03` | High threshold | Read/write | 80 C (`0x5000`, normal format) |

There is **no device ID, manufacturer ID, CRC, EEPROM, or per-address software reset register**. A plausible configuration word establishes interface compatibility, not unique identification as TMP102 versus TMP112. Device model is an application selection.

The only special reset command is I2C **general-call address `0x00`, payload `0x06`**. This resets every compatible target on the bus, so it must not be hidden inside a single-device initialization or recovery operation. SMBus Alert Response uses seven-bit address `0x0C`, and arbitration chooses the lowest responding address. These are bus operations, not device register pointers. A host must explicitly own and authorize such shared-bus operations.

## Configuration

| Bits | Mask | Meaning |
| --- | --- | --- |
| 15 OS | `0x8000` | In shutdown, writing one triggers a single conversion; reads zero busy and one ready |
| 14:13 R1:R0 | `0x6000` | Read-only, always binary 11; resolution is not programmable through these bits |
| 12:11 F1:F0 | `0x1800` | Consecutive fault count: 00=1, 01=2, 10=4, 11=6 |
| 10 POL | `0x0400` | Zero active-low ALERT; one active-high |
| 9 TM | `0x0200` | Zero comparator, one interrupt |
| 8 SD | `0x0100` | One shutdown; active conversion completes before shutdown |
| 7:6 CR1:CR0 | `0x00C0` | 00=0.25Hz, 01=1Hz, 10=4Hz, 11=8Hz |
| 5 AL | `0x0020` | Read-only comparator status; depends on POL, unaffected by TM |
| 4 EM | `0x0010` | Zero normal 12-bit encoding; one extended 13-bit encoding |
| 3:0 | `0x000F` | Reserved, zero |

The persistent writable mask is `0x1FD0`; the OS command adds `0x8000`. Ordinary read-modify-write must clear the sampled OS bit before writing, otherwise an idle shutdown device can accidentally start a conversion. Configuration verification should compare persistent writable bits, not OS or AL. Preserve unrelated persistent settings when changing one field.

Switching EM changes the interpretation of **both thresholds**. Preserve their Celsius values by decoding in the old format and re-encoding in the new format; reject values that cannot be represented. Multiple writes are not atomic, and a failed transaction can leave partial state. Only report success after all required writes succeed, and document/recover from partial failures.

**EM transition caution:** TI support confirmed that changing configuration during a running conversion can produce a temperature word with the new EM marker but the old temperature encoding. Checking bit 0 alone cannot reject that word; decoding it can be wrong by a factor of two. TI recommends first setting SD while preserving the old configuration, waiting 35 ms for any conversion to finish, then applying the new settings. After changing EM, obtain a completed conversion in the new mode before exposing a managed sample. This behavior is documented in the [TMP112 TI engineer response](https://e2e.ti.com/support/sensors-group/sensors/f/sensors-forum/526611/possible-bug-in-the-tmp112-em-mode), with a corresponding [TMP102 discussion](https://e2e.ti.com/support/sensors-group/sensors/f/sensors-forum/503810/tmp102-takes-5-10-seconds-before-giving-valid-13-bits-temperature-reading); offline HTML snapshots are under `ti-e2e/`. A delay after writing SD and the new EM together does not implement the recommended sequence.

## Temperature encoding

Normal mode uses a signed 12-bit two's-complement value in bits 15:4. Extended mode uses signed 13-bit two's complement in bits 15:3. Both have a 0.0625 C LSB. Sign extension should be explicit and portable; avoid left-shifting negative signed integers or relying on implementation-defined right shifts.

**Temperature register bit 0 reports the sample's encoding**: zero normal, one extended. Threshold registers have zero unused low bits, including bit 0 in extended mode. Thus the temperature can be decoded from its own mode marker, while threshold decoding requires EM from configuration.

| Temperature | Normal register | Extended temperature register | Extended threshold register |
| --- | --- | --- | --- |
| 25 C | `0x1900` | `0x0C81` | `0x0C80` |
| -0.0625 C | `0xFFF0` | `0xFFF9` | `0xFFF8` |
| -25 C | `0xE700` | `0xF381` | `0xF380` |
| 125 C | `0x7D00` | `0x3E81` | `0x3E80` |

Representable numbers are -128 through 127.9375 C in normal format and -256 through 255.9375 C in extended format. These **encoding bounds are not operating ratings**. Catalog recommended operating temperature is -40 through 125 C. Threshold APIs should validate finite input, define rounding, and reject overflow rather than wrap it.

## Conversion timing and alert behavior

| Part / document | Typical conversion | Maximum conversion |
| --- | --- | --- |
| TMP102, SBOS397I | 10 ms | 15 ms |
| TMP112 catalog, SBOS473L | 10.25 ms | 11.25 ms |
| TMP102-Q1, SBOS702E | 26 ms | 35 ms |
| TMP112-Q1, SLOS887H | 26 ms | 35 ms |
| TMP112D-Q1, SLOS887H | 10.25 ms | 11.25 ms |

The catalog TMP112 narrative rounds conversion time to 10 ms; use the electrical table for the precise bound. Older drivers often assume 26/35 ms. A 35 ms minimum conversion guard accommodates legacy and Q1 parts; allow additional timeout margin and use OS polling for one-shot completion. Do not equate the conversion interval (4000/1000/250/125 ms) to conversion execution time. A fresh post-power-up temperature is not available immediately.

OS is specified as conversion-ready status for the one-shot/shutdown procedure, not as a universal continuous-mode data-ready flag. An SD write lets an already running conversion finish. TI's [shutdown sequencing explanation](https://e2e.ti.com/support/sensors-group/sensors/f/sensors-forum/571503/tmp112-time-to-get-a-brief-moment-data-in-low-power-comsumption) explicitly waits one maximum conversion time after requesting shutdown before requesting a new one-shot. Preserve that settling time when changing EM; an immediate OS observation during the continuous-to-shutdown transition is not a substitute for this conservative sequence.

In comparator mode, ALERT asserts at or above THIGH after the programmed consecutive-fault count, and clears below TLOW after the same count. Normalize AL as `alertActive = (AL == POL)` after converting each bit to a boolean. AL describes comparator state even in interrupt mode; it is **not the interrupt latch/pin state**.

In interrupt mode, a high crossing asserts the pin; after acknowledgement, a qualifying low crossing can assert it again. Register reads may acknowledge the interrupt. Current datasheets are internally inconsistent: their short interrupt-mode description says a temperature-register read clears the pin, while the detailed threshold-register section says any register read, successful SMBus alert response, or shutdown clears it. Treat polling/status/register-dump reads as potentially acknowledging the interrupt. Do not promise nondestructive interrupt-pin diagnostics based on AL.

The sensor's SMBus timeout resets the serial interface after a line is held low for approximately 30–40 ms; it is not a reset of all user settings. Keep I2C transfers bounded. Use normal 100kHz or 400kHz communication for common cross-platform adapters; special high-speed mode requires a master-code sequence and must not be assumed from a frequency setting alone.

## Address and model differences

Classic SOT563 TMP102/TMP112 use 0x48, 0x49, 0x4A, and 0x4B with ADD0 tied respectively to GND, V+, SDA, and SCL. TMP112 offers tighter accuracy grades than TMP102 but the same register protocol. Accuracy and supply limits depend on suffix; resolution alone does not establish accuracy.

TMP112D in the five-pin X2SON address-select package instead uses **0x40–0x43**, with no ALERT pin. Catalog fixed-address X2SON TMP112D0/D1/D2/D3 use 0x48/0x49/0x4A/0x4B and provide ALERT. Automotive suffix numbering differs: SLOS887H lists D1/D2/D3/D4 for the corresponding fixed-address variants. Refer to the exact package/ordering table rather than inferring capability from the short name. The new TMP112D-Q1 permits a wider supply range than the classic devices; this is an electrical distinction, not an extra register command.

## Official code and repository research

* [TI Linux kernel](https://github.com/TexasInstruments/ti-linux-kernel): immutable snapshot `fff856a50d43288671eaa733f9f59aa25f2fed77` from `ti-linux-6.18.y`. The archived `tmp102.c`, `lm75.c`, `lm75.h`, hwmon documentation, and devicetree bindings provide register definitions and Linux usage. TI's TMP112 product page points to the LM75 driver. These are reference files under their original GPL/SPDX terms; **they are not linked into this library or relicensed by the repository's root license**.
* [TI SBOC486 Arduino example](https://www.ti.com/tool/download/SBOC486): version 01.00.00.00, released 2017-10-22. `sboc486.zip` is the original TI distribution including schematic and sketch; `sboc486-example.ino` is extracted unchanged. The legacy `/lit/zip/sboc486` link now returns an HTML migration page, archived separately. The actual ZIP was fetched from TI's download host and validated as a ZIP.
* [TI MAVRK TMP112 reference](https://software-dl.ti.com/analog/analog_public_sw/MAVRK/latest/API_Documentation/Advanced_Documentation/html/_t_m_p112___temp___sensor_8h.html): archived header source, register enums, initialization, and PC interface command documentation. These are historical board-protocol examples; PC commands are **MAVRK commands**, not additional sensor commands. Source comments and original terms remain intact.
* [TI ASC Studio TMP102](https://www.ti.com/tool/ASC-STUDIO-TMP102) and [TMP112](https://www.ti.com/tool/ASC-STUDIO-TMP112) are official configuration/code-generation tools. No standalone public TMP102/TMP112 repository for ASC was identified in this search.

The GitHub API repository inventory covers public repositories returned for TexasInstruments and TexasInstruments-Sandbox. `ti-repository-search.json` records recursive filename searches for TMP102/TMP112 in TI Linux, SimpleLink F2/F3, MSPM0, MCU+ core, and the TI environmental-sensors repository. Truncated large trees are identified honestly; `ti-repository-subtree-search.json` additionally checks SDK source and example subtrees. No matching filenames were found in the completely returned SDK source trees. Filename searches do not prove that embedded snippets are absent from unrelated files. The environmental-sensors repository currently contains TMP118 and humidity examples, not these devices.

Search-engine queries also covered TI GitHub and `git.ti.com` with both part names. The TI cgit index was reachable through browsing, but target repository/search URLs returned access failures/HTTP 403 during this session. The current TI-owned GitHub Linux mirror supplies the relevant register drivers. This is a reproducible survey of relevant official repositories and artifacts, **not a claim to have exhaustively inspected every branch and file in every TI repository**.

## Archive terms

TI datasheets, TI application notes, example archives, and MAVRK HTML retain their original copyright notices and usage terms. Linux reference source retains its original license headers, with the upstream COPYING and GPL-2.0 text alongside it. This archive is documentation/reference material, excluded from the library build. Consult each artifact's original terms before redistributing it independently.
