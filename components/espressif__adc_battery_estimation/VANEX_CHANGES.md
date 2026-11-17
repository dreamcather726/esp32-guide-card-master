# Local Battery Component

Vendored from the project's Espressif adc_battery_estimation dependency (see
idf_component.yml and license.txt). The existing ESP-IDF ADC bitwidth
compatibility definitions are retained.

`adc_battery_estimation_get_sample` exposes the calibrated ADC voltage,
divider-adjusted battery voltage, instantaneous curve estimate and filtered
capacity from one sampling pass. The original capacity API remains compatible.
The voltage curve and ADC calibration are unchanged. The opt-in
`allow_bidirectional_capacity` setting keeps low-pass filtering but disables
direction locking on boards without a dedicated charge-status GPIO. Its default
is false, preserving the dependency's behavior on other boards.

This local component overrides the managed component of the same name so these
changes survive dependency downloads. Do not edit managed_components directly.
