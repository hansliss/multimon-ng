## Changes in this fork
The POCSAG decoder has been completely rewritten to ensure that it reliably decodes the protocol according to the standard. The "brute force" error correction is unchanged, but there are additional/redundant functions to calculate CRC and parity - because we can.
While doing this, we took a few shortcuts, mainly that the guessing of message types was removed, and locked to Numeric for Function 0 and Alpha for function 3. Function 1 & 2 is now interpreted as Binary, yielding a hex dump in the log as well as an attempt at Alpha decode.

Some of the textual representations of control characters have been replaced with shorter C-style strings, for readability.

NUL and some other termination characters at the end of Alpha strings are now removed, as specified in the standard.

Logging is now done in multiple ways:
* The original "verbprintf()" logging to stdout is intact, but has been complemented with a file log that includes today's day in the file name, to produce a new logfile every day. See TODO.
* There is a debug log that logs received POCSAG words including frame and word-within-frame number, as well as CRC and parity check results, end of batch, acquired sync, and the received message. This is useful for verifying that the decoder logic is working properly.
* There is one additional log, a CSV file that just contains all received raw words, frame number word-in-frame, crc check and parity check, comma-separated. From this you can extract sequences of words for analysis with other programs.

The latter logfiles are optional ("-D" and "-W", respectively) and they both contain timestamps on every line.

### TODO
* Improve the logging options to better accommodate running multimon-ng (for POCSAG) as a background daemon.
* Implement a UDP listener and make multimon-ng accept sample rate as a command-line option, to make it possible to receive data directly from a UDP radio/audio source.

2021, Hans Liss (Hans@Liss.pp.se) & Fredrik Liss
---------------------------------------------------
multimon-ng is the successor of multimon. It decodes the following digital transmission modes:

- POCSAG512 POCSAG1200 POCSAG2400
- FLEX
- EAS
- UFSK1200 CLIPFSK AFSK1200 AFSK2400 AFSK2400_2 AFSK2400_3
- HAPN4800
- FSK9600 
- DTMF
- ZVEI1 ZVEI2 ZVEI3 DZVEI PZVEI
- EEA EIA CCIR
- MORSE CW
- X10

multimon-ng can be built using either qmake or CMake:
```
mkdir build
cd build
qmake ../multimon-ng.pro
make
sudo make install
```
```
mkdir build
cd build
cmake ..
make
sudo make install
```

The installation prefix can be set by passing a 'PREFIX' parameter to qmake. e.g:
```qmake multimon-ng.pro PREFIX=/usr/local```

So far multimon-ng has been successfully built on Arch Linux, Debian, Gentoo, Kali Linux, Ubuntu, OS X, Windows and FreeBSD.
(On Windows using the Qt-MinGW build environment, as well as Cygwin and VisualStudio/MSVC)

Files can be easily converted into multimon-ng's native raw format using *sox*. e.g:
```sox -t wav pocsag_short.wav -esigned-integer -b16 -r 22050 -t raw pocsag_short.raw```
GNURadio can also generate the format using the file sink in input mode *short*. 

You can also "pipe" raw samples into multimon-ng using something like
```sox -t wav pocsag_short.wav -esigned-integer -b16 -r 22050 -t raw - | ./multimon-ng -```
(note the trailing dash)

As a last example, here is how you can use it in combination with RTL-SDR:
```rtl_fm -f 403600000 -s 22050 | multimon-ng -t raw -a FMSFSK -a AFSK1200 /dev/stdin```

Packaging
---------

```
qmake multimon-ng.pro PREFIX=/usr/local
make
make install INSTALL_ROOT=/
```
