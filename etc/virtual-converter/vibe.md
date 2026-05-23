The VirtualConverter (vconv) models a DC-DC synchronous buck or boost converter.
It models the half-bridge MOSFETs (ideal, zero loss), the input and output electrolytic caps and
the coil (ideal).

Input power source: a current source with clamped voltage to model a simplified solar panel.
Output power sink: a constant voltage source as a simplified model for a battery.

The converter model simulates a mock converter for converter firmware tests.
The signal inputs from the firmware are:
binary PWM Gate drive signals for LS and HS
-or-
PWM counts (resolution, LS counts, HS counts).

This still needs to be discussed. binary PWM gate drive signals seems more natural, whereas PWM counts are directly
available in the firmware.

The outputs are mock ADC values:
Vin, Vout, Iout.

Model details:
* DCM/CCM mode
* Reverse coil current
* Waveform of coil current
* Voltage ripple
* Solar model: Isc/Voc

For Later:
- parasitics for passive components
- mosfet power loss
- other power sources / sinks

Do you think its better to first stick to the buck case and then later do boost or both together?
Is this feasible? Where do you see difficulties? Are there relaxations/simplifications to make it easier to implement?
Can we use existing software?
