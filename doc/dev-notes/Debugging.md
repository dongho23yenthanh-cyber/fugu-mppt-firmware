* devices store core dumps on panics, can be retrieved with `coredump` command
* setup a mqtt log recorder and check the log to reconstruct what happended when
* you can estimate the time of the last crash by looking at timestamps and the `N=` from the statusline. if it flips the
  converter has rebooted in between two log lines