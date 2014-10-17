# Windows port of rcopy.

rcopy is included in librdmacm example. 

https://www.openfabrics.org/downloads/rdmacm/README

## How to build

Edit `vcbuild.bat` 

    > vcbuild.bat    

VS Solution file will be generated.

## How to use

    # server on win
    > bin/rcopy.exe

    # client from linux
    $ rcopy myfile.dat SERVER_ADDR:c/path/to/file

## Issue(s)

* Close file correctly.
* Performance may be limited to disk speed.
 
## Copyrights

Licensed under OpenIB.org BSD license.





