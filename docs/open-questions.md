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
For either scope, record the exact configuration and run the relevant physical
checks for every behavior the claim includes.

**Example.** A TMP112D address-select X2SON at `0x40` responds on I2C. That
fixture can cover thermometry and address selection. The package lacks an
ALERT pin, so an ALERT-pin claim needs a different fixture. A response alone
also cannot identify the fitted model.

| Option | Pros | Cons |
| --- | --- | --- |
| Qualify the first consumer's exact configuration, then add others as needed | Produces a usable result with a small fixture and a clear claim boundary. | A consumer that uses multiple configurations still needs a pass for each one it uses. |
| Qualify a cross-package matrix in one campaign, reusing the same reference, logger and analyzer | Covers a wider support claim and catches fixture-specific differences before shipping any of those configurations. | Needs more boards, strap configurations and bench time. Each result still applies to its recorded configuration. |

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
| Use shutdown and call `readBlocking()` in a simple owning loop | A short call sequence can obtain a confirmed conversion in a loop that can wait. | The caller waits during conversion. The polling budget can overrun by the last CONFIG and TEMP callbacks, so include their bounded time in the caller's budget. |
| Use shutdown and request a one-shot through the cooperative owner | Obtains the same confirmed conversion while the owner can schedule other peripherals during the wait. | The caller must schedule polls and consume the terminal result. Both one-shot paths still need time for conversion and transport. |

Use continuous mode when a recent observation satisfies the requirement.
For a conversion required after a specific event, retire any earlier pending
conversion before starting a new one. Require a successful result with
`freshConversion` and report a late or failed request explicitly. Apply
the chosen age limit to any permitted cached fallback.
[Sample provenance and age](field-helpers.md) ·
[Owner operations](owner-operations.md)

## How should the product detect a temperature limit?

**Decision for the product owner.** Choose whether the consumer needs a
polled temperature state, a wire indicating threshold state, or an event
latched until the owner services it. Identify the package and the actual
wiring before committing to a GPIO path. Each option needs a maximum response
time, specified thresholds and a stated response to unavailable readings.
Measure at the selected conversion rate and with the intended bus load.

**Example.** With comparator mode, warming above a high limit asserts ALERT
and cooling below the low limit releases it. In interrupt mode the owner must
account for reads that acknowledge the latched event. A diagnostic register
dump between an assertion and its intended handler could clear the latch
first.

| Option | Pros | Cons |
| --- | --- | --- |
| Compare polled temperatures in the application, with separate high and low thresholds if hysteresis is needed | Works without an ALERT GPIO and has no interrupt-acknowledgment path. | Detection depends on the conversion and polling cadence. A brief excursion can occur between reads. |
| Wire ALERT and use comparator mode | Gives a hardware threshold indication with hysteresis and simpler servicing. An awake application can latch a captured GPIO edge if it needs to remember the event. | Needs an ALERT-capable package and a GPIO. A software latch depends on reliable GPIO capture through the shortest relevant excursion. |
| Wire ALERT and use interrupt mode with one owner controlling reads | Retains an event until an acknowledging read so software can service it asynchronously. | Ordinary register reads can acknowledge the event. The owner must coordinate diagnostics and distinguish a pin observation from comparator status. |

Use polling when its response time meets the requirement. Choose a wired
comparator for a threshold-state signal, or an interrupt when a latch is
needed. A shared SMBus Alert Response route requires its own arbitration
evidence. [Interrupt bench procedure](hardware-validation.md) ·
[SMBus Alert Response](bus-operations.md)
