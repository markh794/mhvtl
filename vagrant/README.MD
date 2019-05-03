# Testing with Vagrant
To test mhvtl on different distros, we use vagrant to spin up virtual boxes with different distros and install mhvtl from source on each box.

## Prerequisites

To be able to test with Vagrant, you need:

- Vagrant (tested with 2.2.4)
- Virtualbox (tested with 5.2.26)

## Test in different Operating Systems

Run any of the following commands to test the code on different GNU/Linux distributions.

```
Vagrant up ubuntu
```

or

```
Vagrant up centos
```

or

```
Vagrant up opensuse
```
**_NOTE:_** OpenSUSE in currently not working

The test will build and install mhvtl from source in the virtual machine. If the output looks something like this, then the installation went fine:
```
...
    ubuntu: Show your tape libraries now!
    ubuntu: [2:0:0:0]    disk    VBOX     HARDDISK         1.0   /dev/sda   /dev/sg0
    ubuntu: [2:0:1:0]    disk    VBOX     HARDDISK         1.0   /dev/sdb   /dev/sg1
    ubuntu: [3:0:0:0]    mediumx STK      L700             0106  -          /dev/sg7
    ubuntu: [3:0:1:0]    tape    IBM      ULT3580-TD5      0106  -          /dev/sg6
    ubuntu: [3:0:2:0]    tape    IBM      ULT3580-TD5      0106  -          /dev/sg11
    ubuntu: [3:0:3:0]    tape    IBM      ULT3580-TD4      0106  -          /dev/sg4
    ubuntu: [3:0:4:0]    tape    IBM      ULT3580-TD4      0106  -          /dev/sg8
    ubuntu: [3:0:8:0]    mediumx STK      L80              0106  -          /dev/sg3
    ubuntu: [3:0:9:0]    tape    STK      T10000B          0106  -          /dev/sg5
    ubuntu: [3:0:10:0]   tape    STK      T10000B          0106  -          /dev/sg2
    ubuntu: [3:0:11:0]   tape    STK      T10000B          0106  -          /dev/sg9
    ubuntu: [3:0:12:0]   tape    STK      T10000B          0106  -          /dev/sg10
    ubuntu: Found some virtual tapes, success!
```

### Cleanup

When you are done, and you want to remove the box, run the following command:

```
Vagrant destroy -f
```
