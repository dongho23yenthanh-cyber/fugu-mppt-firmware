
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
