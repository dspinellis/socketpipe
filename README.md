[![Build Status](https://travis-ci.org/dspinellis/socketpipe.svg?branch=master)](https://travis-ci.org/dspinellis/socketpipe)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/8492/badge.svg)](https://scan.coverity.com/projects/dspinellis-socketpipe)

_Socketpipe_ connects over a TCP/IP socket a _remote command_ specified to a local _input generation command_ and/or a local _output processing command_.  The input and output of the _remote command_ are appropriately redirected so that the remote command's input will come from the local _input generation command_ and the remote command's output will be sent to the local _output processing command_.  The remote command is executed on the machine accessed through the _login command_.  The _socketpipe_ executable should be available through the execution path in the remote machine.  The braces used for delimiting the commands and their arguments should be space-separated and can be nested.  This feature allows you to setup complex and efficient topologies of distributed communicating processes.

Although the initial _socketpipe_ communication setup is performed through client-server intermediaries such as _ssh(1)_ or _rsh(1)_, the communication channel that _socketpipe_ establishes is a direct socket connection between the local and the remote commands.  Without the use of _socketpipe_, when piping remote data through _ssh(1)_ or _rsh(1)_, each data block is read at the local end by the respective client, is sent to the remote daemon and written out again to the remote process.  The use of _socketpipe_ removes the inefficiency of the multiple data copies and context switches and can in some cases provide dramatic throughput improvements.  On the other hand, the confidentiality and integrity of the data passing through _socketpipe_'s data channel is not protected; _socketpipe_ should therefore be used only within a confined LAN environment.  (The authentication process uses the protocol of the underlying login program and is no more or less vulnerable than using the program in isolation; _ssh_(1) remains secure, _rsh_(1) continues to be insecure.)

# Examples
```
socketpipe -i { tar cf - / } -l { ssh remotehost } -r { dd of=/dev/st0 bs=32k }
```
Backup the local host on a tape drive located on _remotehost_.
```
socketpipe -l { ssh remotehost } -r { dd if=/dev/st0 bs=32k } -o { tar xpf - /home/luser }
```
Restore a directory using the tape drive on the remote host.
```
socketpipe -i { tar cf - / } -l { ssh remotehost } -r { bzip2 -c } -o { dd of=/dev/st0 bs=32k }
```
Backup the local disk on a local tape, compressing the data on the (presumably a lot more powerful) _remotehost_.


# Project home
You can download the source and executables from the
[project's page](http://www.spinellis.gr/sw/unix/socketpipe).

# Building
* To build the program under Unix, Linux, Cygwin run ```make```
* To build the program under Microsoft C/C++ run ```nmake /f Makefile.mak```
