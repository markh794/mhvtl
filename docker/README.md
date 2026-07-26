# Why do this

Docker uses the host kernel to launch and function, thus we cannot really "dockerize" MHVTL, but we can quickstart the process by just installing the kernel module. 

Why? Because sometimes it's best to keep things minimal on a device (or at least I like to think so).
Why alma 8 ? It should work with other versions too, but I happened to need alma 8 because other tools I use require a VTL and only works with alma 8.

To make a Docker container capable of running MHVTL, we need to make a few exceptions; which is bad security-wise, of course. Docker should not be used like that unless it's for testing or quick development (which is the only reason I made this in the first place).

MHVTL is a special case because it interacts with the kernel and creates virtual tape devices. This means that a fully isolated container is not possible. Thus, the host machine will "see" the virtual tape devices created by MHVTL, meaning that these devices are not isolated from the host like they would be with a typical containerized application.

To do so the docker will : 
- use higher privileges to access `/lib/modules` and access the kernel module. This is required because the kernel module cannot run inside the container itself. It must be loaded by the host kernel.
- build all the user-space tools (basically doing `make` in `/usr` and in `/etc`).

Logically, it should also makes it easier to test different MHVTL versions without modifying the host installation.

Finally, you can connect to the Docker container and it will work. Just so you know, the higher privileges allow the host to access the VTL.

# Quickstart

My Dockerfile contains an example configuration. Just run the commands below and MHVTL should be working. By default, my Dockerfile creates a test directory located at `/etc/mhvtl/test` and `/opt/mhvtl/test`.

## 1. Installing the kernel module

```sh
pwd # /mhvtl
sudo make kernel
```

## 2. Building the container

```sh
pwd # /your/path/mhvtl/docker
docker compose up --build -d
```

## 3. Testing MHVTL in a container

```sh
lsscsi -g # Nothing appears except the host's disk
```

```sh
/mhvtl/etc/generate_device_conf \
    -f \
    -D /etc/mhvtl/test \
    -H /opt/mhvtl/test
```

```sh
/mhvtl/etc/generate_library_contents \
    -f \
    -D /etc/mhvtl/test \
    -C /etc/mhvtl/test
```

```sh
systemctl start mhvtl.target
lsscsi -g # All devices defined in /etc/mhvtl/test/device.conf should appear
```

To stop MHVTL and remove the tapes:

```sh
systemctl stop mhvtl.target
lsscsi -g # No MHVTL devices should appear
```

Below is the terminal output from a successful test run.
```
└─(16:11:34 on master ✭)──> docker exec -it mhvtl bash 
[root@mhvtl mhvtl]# lsscsi 
[N:0:0:1]    disk    Micron MTFDKCD512TFK__1                    /dev/nvme0n1
[root@mhvtl mhvtl]# /mhvtl/etc/generate_device_conf \
>     -f \
>     -D /etc/mhvtl/test \
>     -H /opt/mhvtl/test
===> Generating: /etc/mhvtl/test/device.conf ...
[root@mhvtl mhvtl]# /mhvtl/etc/generate_library_contents \
>     -f \
>     -D /etc/mhvtl/test \
>     -C /etc/mhvtl/test
===> Generating: /etc/mhvtl/test/library_contents.10 ...
===> Generating: /etc/mhvtl/test/library_contents.30 ...
[root@mhvtl mhvtl]# lsscsi -g
[N:0:0:1]    disk    Micron MTFDKCD512TFK__1                    /dev/nvme0n1  -        
[root@mhvtl mhvtl]# systemctl start mhvtl.target
[root@mhvtl mhvtl]# lsscsi -g
[0:0:0:0]    mediumx STK      L700             0108  -          -        
[0:0:1:0]    tape    IBM      ULT3580-TD8      2160  -          -        
[0:0:2:0]    tape    IBM      ULT3580-TD8      2160  -          -        
[0:0:3:0]    tape    IBM      ULT3580-TD6      2160  -          -        
[0:0:4:0]    tape    IBM      ULT3580-TD6      2160  -          -        
[0:0:8:0]    mediumx STK      L80              0108  -          -        
[0:0:9:0]    tape    STK      T10000B          550V  -          -        
[0:0:10:0]   tape    STK      T10000B          550V  -          -        
[0:0:11:0]   tape    STK      T10000B          550V  -          -        
[0:0:12:0]   tape    STK      T10000B          550V  -          -        
[N:0:0:1]    disk    Micron MTFDKCD512TFK__1                    /dev/nvme0n1  -   
```

Author: Matthias LAPU
