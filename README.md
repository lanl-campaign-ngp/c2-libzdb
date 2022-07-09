**A helper library able to translate ZFS filenames to the underlying disk LBAs that are mapped to the files**

C2 LibZDB
================

```
XX              XXXXX XXX         XX XX           XX       XX XX XXX         XXX
XX             XXX XX XXXX        XX XX           XX       XX XX    XX     XX   XX
XX            XX   XX XX XX       XX XX           XX       XX XX      XX XX       XX
XX           XX    XX XX  XX      XX XX           XX       XX XX      XX XX       XX
XX          XX     XX XX   XX     XX XX           XX XXXXX XX XX      XX XX       XX
XX         XX      XX XX    XX    XX XX           XX       XX XX     XX  XX
XX        XX       XX XX     XX   XX XX           XX       XX XX    XX   XX
XX       XX XX XX XXX XX      XX  XX XX           XX XXXXX XX XX XXX     XX       XX
XX      XX         XX XX       XX XX XX           XX       XX XX         XX       XX
XX     XX          XX XX        X XX XX           XX       XX XX         XX       XX
XX    XX           XX XX          XX XX           XX       XX XX          XX     XX
XXXX XX            XX XX          XX XXXXXXXXXX   XX       XX XX            XXXXXX
```

LibZDB is developed as part of C2 under U.S. Government contract 89233218CNA000001 for Los Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC for the U.S. Department of Energy/National Nuclear Security Administration. See the accompanying LICENSE.txt for further information. ZDB is a component of OpenZFS, an advanced file system and volume manager which was originally developed for Solaris and is now maintained by the OpenZFS community. Visit [openzfs.org](https://openzfs.org/) for more information on this open source file system.

# ZFS

We focus on ZFS 0.8.3 for now.

# Software requirements

Compiling LibZDB currently requires zfs, g++, cmake, and make. On Ubuntu, one may use the following commands to prepare the programming environment for LibZDB.

```bash
sudo apt-get install gcc make cmake cmake-curses-gui \
  libzfslinux-dev zfsutils-linux
```

For Ubuntu 20.04.4, this will install g++ 9.4.0, cmake 3.16.3, make 4.2.1, and zfs 0.8.3.

## ZFS headers

On Ubuntu, installing libzfslinux-dev will not install zfs_ioctl.h which is required by libZDB. To resolve this issue, one can manually install the header from the zfs source tree. Make sure to use zfs 0.8.3.

```bash
cd /usr/include/libzfs/sys
sudo wget https://raw.githubusercontent.com/openzfs/zfs/zfs-0.8.3/include/sys/zfs_ioctl.h
```

# Building LibZDB

After all software requirements are installed, one can use the following to configure and build libZDB.

```bash
git clone https://github.com/lanl-future-campaign/c2-libzdb.git
cd c2-libzdb
mkdir build
cd build
cmake -DSPL_INCLUDE_DIR=/usr/include/libspl \
-DZFS_INCLUDE_DIR=/usr/include/libzfs \
-DZFS_LIBRARY_DIR=/lib \
-DBUILD_SHARED_LIBS=ON \
-DCMAKE_BUILD_TYPE=Release ..
make
```

# Example Zpool configuration

```bash
# sudo zpool status
  pool: mypool
 state: ONLINE
  scan: scrub repaired 0B in 0 days 00:00:00 with 0 errors on Sun Apr 10 00:24:01 2022
config:

	NAME                STATE     READ WRITE CKSUM
	mypool              ONLINE       0     0     0
	  mirror-0          ONLINE       0     0     0
	    /var/dsk/disk1  ONLINE       0     0     0
	    /var/dsk/disk2  ONLINE       0     0     0
	  mirror-1          ONLINE       0     0     0
	    /var/dsk/disk3  ONLINE       0     0     0
	    /var/dsk/disk4  ONLINE       0     0     0

errors: No known data errors
```

# Example LibZDB output

```txt
obj=2 dataset=mypool path=/file1 type=19 bonustype=44

    Object  lvl   iblk   dblk  dsize  dnsize  lsize   %full  type
         2    1   128K    512    512     512    512  100.00  ZFS plain file (K=inherit) (Z=inherit)
                                               176   bonus  System attributes
	dnode flags: USED_BYTES USERUSED_ACCOUNTED USEROBJUSED_ACCOUNTED 
	dnode maxblkid: 0
	uid     0
	gid     0
	atime	Thu Jan 20 18:47:44 2022
	mtime	Sat Jan  8 17:48:35 2022
	ctime	Sat Jan  8 17:48:35 2022
	crtime	Sat Jan  8 17:48:35 2022
	gen	10
	mode	100644
	size	11
	parent	34
	links	1
	pflags	840800000004
Indirect blocks:
               0 L0 0:d400:200 200L/200P F=1 B=10/10 cksum=cce12adf:660b184684:199c9f1c812e:451a630525280

		segment [0000000000000000, 0000000000000200) size   512
```

# References

[ZFS Cheat Sheet by Serge Y. Stroobandt](https://hamwaves.com/zfs/en/zfs.a4.pdf)

[ZFS without Tears](https://www.csparks.com/ZFS%20Without%20Tears.md)
