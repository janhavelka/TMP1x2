# Open questions

These are choices for the first physical qualification and for products that
use TMP1x2. Existing [integration contracts](integration.md) describe what
the library provides today. See [validation results](validation.md) for
executed checks and the [hardware checklist](hardware-validation.md) for the
remaining bench work.

## Which configuration should be qualified first?

**Decision for the bench owner.** Name the first board revision, fitted
sensor and package, address strap, and framework. Decide whether the first
claim should cover one product configuration or a wider library matrix.

**Example.** A TMP112D address-select X2SON at `0x40` responds on I2C. That
fixture can cover thermometry and address selection. The package lacks an
ALERT pin, so an ALERT-pin claim needs a different fixture. A response alone
also cannot identify the fitted model.

| Option | Pros | Cons |
| --- | --- | --- |
| Qualify the first consumer's exact configuration, then add others as needed | Produces a usable result with a small fixture and a clear claim boundary. | Other packages, straps, frameworks and ALERT routes need their own evidence. |
| Qualify a cross-package matrix in one campaign | Covers more library use cases and catches fixture-specific differences earlier. | Needs more boards, strap configurations and time with the temperature reference and analyzer. |

Start with the first consumer's BOM when one is available. Record the fitted
part and publish the evidence per configuration.
[Model and address selection](integration.md) ·
[Bench record requirements](hardware-validation.md)

## Does the application need a new conversion for each reading?

**Decision for the product owner.** Set the maximum usable age and decide
which operations need a conversion initiated for that request. Use one policy
for each consumer action, including what happens when a new reading is late.

**Example.** A status display can show the last trustworthy temperature
within its age limit. A reading taken after a fixture step may need a new
conversion. Reading the continuous-mode register immediately after the step
can return the previous conversion with a new read timestamp. A completed
one-shot provides evidence of a requested conversion. Thermal settling still
needs its own limit.

| Option | Pros | Cons |
| --- | --- | --- |
| Use continuous conversion and poll the latest register | Simple for a trend display or periodic monitoring. The caller can enforce a maximum read age. | The read timestamp does not establish a distinct conversion or when the sensor converted the value. |
| Use shutdown and request a one-shot through the cooperative owner | The returned sample can confirm the requested conversion. The shared bus serves other peripherals during the wait. | The application must allow for conversion latency and handle an expired or failed request. |

Use continuous mode when a recent observation satisfies the requirement.
Choose one-shot where the operation needs a requested conversion, and define
when an older sample is usable. [Sample provenance and age](field-helpers.md) ·
[Owner operations](owner-operations.md)

## How should the product detect a temperature limit?

**Decision for the product owner.** Choose whether the consumer needs a
polled temperature state, a wire indicating threshold state, or an event
latched until the owner services it. Identify the package and the actual
wiring before committing to a GPIO path.

**Example.** With comparator mode, warming above a high limit asserts ALERT
and cooling below the low limit releases it. In interrupt mode the owner must
account for reads that acknowledge the latched event. A diagnostic register
dump between an assertion and its intended handler could clear the latch
first.

| Option | Pros | Cons |
| --- | --- | --- |
| Compare polled temperatures in the application | Works without an ALERT GPIO and has no interrupt-acknowledgment path. | Detection depends on the conversion and polling cadence. |
| Wire ALERT and use comparator mode | Gives a hardware threshold indication with hysteresis and simpler servicing. | Needs an ALERT-capable package, a GPIO and measured threshold timing. |
| Wire ALERT and use interrupt mode with one owner controlling reads | Retains an event until an acknowledging read so software can service it asynchronously. | Ordinary register reads can acknowledge the event. The owner must coordinate diagnostics and distinguish a pin observation from comparator status. |

Use polling when its response time meets the requirement. Choose a wired
comparator for a threshold-state signal, or an interrupt when a latch is
needed. A shared SMBus Alert Response route requires its own arbitration
evidence. [Interrupt bench procedure](hardware-validation.md) ·
[SMBus Alert Response](bus-operations.md)
