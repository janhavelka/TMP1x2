# Documentation

- [2026-10-03 pre-HIL audit](audit-2026-10-03.md): datasheet review, defects,
  regression evidence and remaining hardware gates.
- [Serial HIL runner](hil-runner.md): automated suites, fixture setup and evidence.
- [2026-09-26 audit](audit-2026-09-26.md): sibling parity, confirmed defects and fixes.
- [Library comparison](library-comparison.md): all local standalone I2C libraries
  surveyed and the chosen API/CLI conventions.
- [Feature coverage](feature-coverage.md): chip features, initialization/name parity,
  field helpers and explicit application-owned bus behavior.
- [Field helpers](field-helpers.md): configuration validation, cached samples,
  health snapshots, register readback and temperature units.
- [Shared-bus operations](bus-operations.md): explicit reset and SMBus Alert Response.
- [Integration](integration.md): external bus ownership, callback contracts,
  health, configuration trust and timing.
- [Cooperative owner operations](owner-operations.md): transfer budgets, time,
  cancellation, retained results and scheduler integration.
- [Reference archive](reference/README.md): TI specifications, official source
  survey, register map, quirks, source URLs and checksums.
- [Validation results](validation.md): what was actually tested and build limits.
- [Hardware validation](hardware-validation.md): physical verification procedure.
- [Examples](../examples/README.md): Arduino/native IDF CLI setup and commands.

The repository includes reference PDFs and vendor source snapshots. Binary/vendor
artifacts are deliberately omitted from the small PlatformIO release package;
use this repository checkout to access the complete archive.
