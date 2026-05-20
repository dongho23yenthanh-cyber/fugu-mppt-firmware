# web app editor with upload

Write a command-line tool to upload, download and view firmware configuration files.
Take a look at download.py .
The tool uses discover_scope_servers() to discover chips on the network.
Chips can be filtered by command line argument (regex matching against host or ip).

It then downloads the files on the chip with `download_ftp_tree()`.

The user can provide a local directory to diff the downloaded configurations with.
The user can see the entries with different value and can chose which fields to accept.
Existing comments in the files are preserved.
The script then uploads the changed files to the chip.

Example command:

```
conf-tool.py --hosts '.+' --local-conf config/fmetal
```

# web version

Create a web app to edit a ConfFile set (single page HTML). Users can upload *.zip files (or folders if possible) which
follow
the structure like ../../config/fmetal . Each .conf file gets its own tab.
Inspect each config value, try to find it in the firmware code and figure out unit (if applicable) and a short
description.
Display both in the input form.
When users are done editing, they can download a zip file with the updated configuration files.

* show the data type of each field (string, byte, long, float, ...)
* display the unit inside the input elements, right aligned
* GPIO pins have the unit "GPIO"
* show all fields, even if they are missing in the file. inputs have a background text '<not set>' then.
* missing fields are only written to the files if they already existed
* there is a way to add new entries.
* show the raw contents of the .conf file below
* there is a x -button next to the input that set the value to '<not set>' removing the entry from the file
    * the x-button is only visible if a value is present
* when a value in a tab has been changed, show a small dot next to the tab name
* the input fields have a reasonable width
* when changing or deleting a value display its original value loaded from the file below the input (`was: ...`)
    * when deleting a field with the value 0, make sure that `was: ...` is still shown

* test the page headlessly (jsdom or similar), if you can't report it before continuing

# serial port

An alternative way to the upload: users can feed in data directly from the device over a serial console.
Add a button that lets the user connect to a serial port (baud-rate 115200).
Once connected, the values can be read with the `get-config <file>` command, for example:

```
> get-config board.conf
I (267894) main: received serial command: 'get-config board.conf'
Conf '/littlefs/conf/board.conf:mcu' = 'esp32s3'
Conf '/littlefs/conf/board.conf:skip_assert' = '1'
Conf '/littlefs/conf/board.conf:adc_fake_freq' = '3000'
Conf '/littlefs/conf/board.conf:pwm_freq' = '39000'
I (267988) main: OK: get-config board.conf
```

make sure to wait for the final confirmation `main: OK: get-config <file>`

First, try the communication with the device directly to get an understanding.
If the port is held by a `idf.py monitor` session, kill that session. If the port is held by another process, stop and
report.
You find the device serial port with cu.usbmodem* or cu.usbserial*, or "USB JTAG/serial debug unit"
Then headlessly test it with the Web Serial API. If you can't test, stop and report why.
If you cannot automate the native serial port-chooser dialog and there is no way to grant a physical serial port,
drive it with a mock navigator.serial whose port replays the real device's exact captured output.



# bluetooth

an additional way to read (and write) config files is a BLE GATT Nordic UART Service (NUS) console.
It uses the same protocol as the wird serial console described earlier.
Put another button next to the Serial Connect button to scan for a BLE device, connect to the NUS console and fetch the files.
