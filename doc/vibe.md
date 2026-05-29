

write a new md document describing how the LFP charging works.
make it understandable for someone we cannot read code, don't reference code variables.

List every relevant ConfFile variable:
* name
* unit
* range (numerical or logical, e.g. ibat_max is clamped at iout_max)
* short description

briefly describe the termination logic. you embedded latex.

include the "The tail_c_rate parameter" from Termination.md.
add a chart relative charging current over cell-voltage and draw the termination line. 

include the "Recharge hysteresis (DoD-based release)" from Termination.md.



# more
write a config tool
-html



# differential binary flaser




## filtering
* evaluate the filter pipeline (adc averaging, notch, med3, ewm). Do the components make sense? Is the order correct?
  any other filter recommendation (kalman, multi-pass ewm ..) for the digital control loop? (Vout is the critical control variable)
* when the user connects to the console, i want the charger to print the last 20 warnings and errors
* detect high impedance battery connection



# etc
- review that part of the code
- look for heavy imports, that can be avoided by moving code from headers into .cpp files
- look for storage specifier optimiuatio

# routine
> test? docs? commits?
> 


# adc error

there is currently a regression in the changes that had been made to the ADC code today.
the devices all fail with `E (1920) main: ADC error`. 
We added a ADC watchdog, before we noticed that error. There have been many commits and it got a bit messy.

Relevant git tags are:
* fry-work: flashed it and it worked smoothly.
* fry-adcOk-wifiSOF: adc ok but sometimes wifi stack overflow (disable wifi)
* fry-brk1: (adc error)

So the breaking change must be between fry-brk1 and  fry-adcOk-wifiSOF .
T

fry (currently usb attached) has an issue with the ADC. it is not a hardware
fault.   i think thereis sth wrong with the ina226 recent changes . can you A/B test to find the
breaking change?





It is safe to touch the device, it has solide hardware protection.
Do it on autopilot.