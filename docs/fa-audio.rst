========
fa-audio
========

.. Written in reStructuredText
.. default-role:: literal

-----------------------------------------
Plays FA data as audio to the PC speakers
-----------------------------------------

:Author:            Michael Abbott, Diamond Light Source Ltd
:Date:              2011-05-27
:Manual section:    1
:Manual group:      Diamond Light Source

Synopsis
========
fa-audio [*options*] [*location*]

Description
===========
Plays FA data through the PC speakers using the aplay(1) tool.  The location
name of the archive server is given as an optional command line argument,
possible locations are SR, BR and TS.  A simple command interface is available
with the following commands:

q
    Quit

h
    Shows list of commands (also shown if unrecognised command given)

b *fa-id*
    Select given *fa-id* for playback

v *volume*
    Set volume level to specified dB level.  The nominal volume level of 0dB
    corresponds to a level that is not excessive on BPMs with no signal input,
    position dominated by BPM nose.

    Note that the actual volume level is adjusted, block by block, to avoid
    clipping.

x
    Selects playback of X data in both speaker channels.

y
    Selects playback of Y data in both speaker channels.

a
    Selects playback of X data in left speaker channel, Y data in right speaker
    channel.

i
    Shows currently selected BPM, volumen level and channel selection, together
    with the maximum volume level that can currently be set without clipping
    (computed over the last five seconds).

Options
=======
The following options can be specified on the command line:

-v volume
    Set the initial volume level, the default is 0dB.

-b fa-id
    Set the initial FA id for playback, the default is number 1.

-f
    Normally the location file is looked up in the `python/conf` directory of
    the installation, but if this flag is set the location string is interpreted
    as a file name.

-S server
    Can be used to override the server address in the location file.

See Also
========
aplay(1)
