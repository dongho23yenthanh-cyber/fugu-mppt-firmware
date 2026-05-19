

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

