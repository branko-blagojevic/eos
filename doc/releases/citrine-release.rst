:orphan:

.. highlight:: rst

.. index::
   single: Citrine-Release


Citrine Release Notes
======================

``Version 4 Citrine``

Introduction
------------
This release is based on XRootD V4 and IPV6 enabled.


``v4.3.4 Citrine``
===================

2018-07-04

Bug
----

* [EOS-2686] - DrainFs::UpdateProgress maxing out CPU on PPS
* Fix race conditions and crashes while updating the global config map
* Fix lock order inversion in the namespace prefetcher code leading to deadlocks

New feature
------------

* FUSEX: Add FIFO support

Improvement
------------

* Remove artificial sleep when generating TPC drain jobs since the underlying issue
  is now fixed in XRootD 4.8.4 - it was creating identical tpc keys.
* Replace the use of XrdSysTimer with std::this_thread::sleep_for


``v4.3.3 Citrine``
===================

2018-06-29

Improvement
------------

* FUSEX: Fix issues with the read-ahead functionality
* MGM: Extended the routing functionality to detect online and master nodes with
  automatic stalling if no node is available for a certain route.
* MGM: Fix race condition when updating the global configuration map


``v4.3.2 Citrine``
===================

2018-06-26

Bug
---

* FUSEX: encode 'name' in requests by <inode>:<name>
* MGM: decode 'name' in requests by <inode>:<name>
* MGM: decode routing requests from eosxd which have an URL encoded path name


``v4.3.1 Citrine``
===================

2018-06-25

Bug
---

* FUSEX: make the bulk rm the default
* FUSEX: by default use 'backtace' handler, fusermount -u and emit received signal again.
* FUSEX: use bulk 'rm' only if the '-rf' flag and not verbose option has been selected
* FUSEX: avoid possible dead-lock between calculateDepth and invalidation callbacks


``v4.3.0 Citrine``
===================

2018-06-22

Bug
----

* [EOS-1132] - eosarchived.py, write to closed (log) file?
* [EOS-2401] - FST crash in eos::fst::ScanDir::CheckFile (EOSPPS)
* [EOS-2513] - Crash when dumping scheduling groups for display
* [EOS-2536] - FST
* [EOS-2557] - disk stats displaying for wrong disks
* [EOS-2612] - Probom parsing options in "eos fs ls"
* [EOS-2621] - Concurrent access on FUSE can damage date information (as shown by ls -l)
* [EOS-2623] - EOSXD loses kernel-md record for symbolic link during kernel compilation
* [EOS-2624] - Crash when removing invalid quota node
* [EOS-2654] - Unable to start slave with invalid quota node
* [EOS-2655] - 'eos find' returns different output for dirs and files
* [EOS-2656] - Quota rmnode should check if there is quota node before deleting and not afater
* [EOS-2659] - IO report enabled via xrd.cf but not collecting until enabled on the shell
* [EOS-2661] - space config allows fs.configstatus despite error message

New Feature
-----------

* [EOS-2313] - Add queuing in the central draining


Improvement
-----------

* [EOS-2297] - MGM: "boot time" is wrong, should count from process startup
* [EOS-2460] - MGM should not return
* [EOS-2558] - Fodora 28 rpm packages
* [EOS-2576] - http: x509 cert mapping using legacy format
* [EOS-2589] - git checkout slow
* [EOS-2629] - Make VST reporting opt-in instead of opt-out
* [EOS-2644] - Possibility to configure #files and #dirs on MGM with quarkdb


``v4.2.26 Citrine``
===================

2018-06-20

Bug
---

* [EOS-2662] - ATLAS stuck in stacktrace due to SETV in malloc in table formatter
* [EOS-2415] - Segmentation fault while building the quota table output


``v4.2.25 Citrine``
===================

2018-06-14

Bug
---

* Put back option to enable external authorization library


``v4.2.24 Citrine``
===================

2018-06-13

Bug
----

* [EOS-2081] - "eosd" segfault in sscanf() / filesystem::stat() / EosFuse::lookup
* [EOS-2600] - Clean FST shutdown wrongly marks local LevelDB as dirty

New Feature
-----------

* Use std::shared_timed_mutex for the implementation of RWMutex. This is by default
disabled and can be enabled by setting the EOS_USE_SHARED_MUTEX=1 environment var.

Improvement
-----------

* The FSTs no longer do the dumpmd when booting.


``v4.2.23 Citrine``
===================

2018-05-23

Bug
----

* [EOS-2314] - Central draining traffic is not tagged properly
* [EOS-2318] - Slave namespace failed to boot (received signal 11)
* [EOS-2465] - adding quota node on the master kills the slave (which then bootloops trying to apply the same quota)
* [EOS-2537] - Balancer sheduler broken
* [EOS-2544] - Setting recycle bin size changes inode quota to default.
* [EOS-2564] - CITRINE MGM does not retrieve anymore error messages from FSTs in error.log
* [EOS-2574] - enabling accounting on the slave results in segfault shortly after NS booted
* [EOS-2575] - used space on /eos/<instance>/proc/conversion is ever increasing
* [EOS-2579] - Half of the Scheduling groups are selected for  new file placement
* [EOS-2580] - 'find -ctime' actually reads and compares against 'mtime'
* [EOS-2582] - Access command inconsistencies
* [EOS-2585] - EOSFUSE inline-repair not working
* [EOS-2586] - The client GEOTAG is not taken into account when performing file placement

New Feature
------------

* [EOS-2566] - Enable switch to propagate uid only via fuse

Task
----

* [EOS-2119] - Implement support in central drain for RAIN layouts + reconstruction
* [EOS-2587] - Fix documentation for docker deployment

Improvement
-----------

* [EOS-2462] - improve eos ns output
* [EOS-2571] - Change implementation of atomic uploads`
* [EOS-2588] - Change default file placement behaviour in case of clients with GEOTAG


``v4.2.22 Citrine``
===================

2018-05-03

Bug
----

* [EOS-2486] - eosxd stuck, last message "recover reopened file successfully"
* [EOS-2512] - FST crash around eos::fst::XrdFstOfsFile::open (soon after start, "temporary fix"?)
* [EOS-2516] - "eosd" aborts with std::system_error "Invalid argument" on shutdown (SIGTERM)
* [EOS-2519] - Segmentation fault when receiving empty opaque info
* [EOS-2529] - eosxd: make renice =setpriority() optional, req for unprivileged containers
* [EOS-2541] - (eosbackup halt): wrong timeout and fallback in FmdDbMapHandler::ExecuteDumpmd
* [EOS-2543] - Unable to read 0-size file created with eos touch

New Feature
-----------

* [EOS-1811] - RFE: support for "hard links" in FUSE
* [EOS-2505] - RFE: limit number of inodes for FUSEX cache, autoclean
* [EOS-2518] - EOS WfE should log how long it takes to execute an action
* [EOS-2542] - Group eossync daemons in eossync.target

Improvement
-----------

* [EOS-2114] - trashbin behaviour for new eos fuse implementation
* [EOS-2423] - EOS_FST_NO_SSS_ENFORCEMENT breaks writes
* [EOS-2532] - Enable recycle bin feature on FUSEX
* [EOS-2545] - Report metadata cache statistics through "eos ns" command

Question
--------

* [EOS-2458] - User quota exceeted and user can write to this directory
* [EOS-2497] - Repeating eos fusex messages all over

Incident
--------

* [EOS-2381] - File lost during fail-over ATLAS


``v4.2.21 Citrine``
===================

2018-04-18

Bug
----

* [EOS-2510] - eos native client is not working correctly against eosuser

New
----

* XrootD 4.8.2 readiness and required

``v4.2.20 Citrine``
===================

2018-04-17

Improvements
------------

FST: make the connection pool configurable by defining EOS_FST_XRDIO_USE_CONNECTION_POOL
FUSE: avoid that FUSE calls open in a loop for every write in the outgoing write-back cache if the file open failed
FUSE: remove 'dangerous' recovery functionality which is unnecessary with xrootd 4
FUSE: Try to re-use connections towards the MGM when using the same credential file


``v4.2.19 Citrine``
===================

2018-04-10

Bug
----

* [EOS-2440] - `eos health` is broken
* [EOS-2457] - EOSPPS: several problems with `eos node ls -l`
* [EOS-2466] - 'eos rm' on a file without a container triggers an unhandled error
* [EOS-2475] - accounting: storagecapacity should be sum of storageshares

Task
----

* [EOS-1955] - .xsmap file still being created (balancing? recycle bin?), causes "corrupted block checksum"


``v4.2.18 Citrine``
===================

2018-03-20

Bug
----

* [EOS-2249] - Citrine generation of corrupted configuration
* [EOS-2288] - headroom is not propagated from space to fs
* [EOS-2334] - Failed "proto:" workflow notifications do not end up in either the ../e/.. or ../f/.. workflow queues
* [EOS-2360] - FST aborts with "pure virtual method called", "terminate called without an active exception" on XrdXrootdProtocol::fsError
* [EOS-2413] - Crash while handling a protobuf reply
* [EOS-2419] - Segfault around TableFormatter (when printing FSes)
* [EOS-2424] - proper automatic lock cleanups
* [EOS-2428] - draining jobs create .xsmap files on the source and destination FSTs
* [EOS-2429] - FuseServer does not grant SA_OK permission if ACL only allows to be a writer
* [EOS-2432] - eosapmond init script for CC7 sources /etc/sysconfig/eos
* [EOS-2433] - Wrong traffic accounting for TPC/RAIN/Replication
* [EOS-2436] - FUSEX: permission problem in listing shared folder
* [EOS-2438] - FUSEX: chmod +x does not work
* [EOS-2439] - FUSEX: possible issue with sys.auth=*
* [EOS-2442] - TPC of 0-size file fails

Improvement
-----------

* [EOS-2423] - EOS_FST_NO_SSS_ENFORCEMENT breaks writes
* [EOS-2430] - fusex cache should not use /var/eos

Question
--------

* [EOS-2431] - fusex cache cleanup


``v4.2.17 Citrine``
===================

2018-03-15

Bug
---

* [EOS-2292] - eosd 4.2.4-1 segmentation fault in SWAN
* [EOS-2322] - eosd 4.2.4-1 segmentation fault on swan003
* [EOS-2388] - Fuse::utimes only honours posix permissions, but not ACLs
* [EOS-2402] - FST abort in eos::fst::FmdDbMapHandler::ResyncAllFromQdb (EOSPPS)
* [EOS-2403] - eosd 4.2.4-1 SegFaults on swan001
* [EOS-2404] - eosd 4.2.4-1 SegFaults on swan002

Improvement
-----------

* [EOS-2389] - Classify checksum errors during scan
* [EOS-2398] - Apply quota settings relativly quick in time on the FUSEX clients
* [EOS-2408] - Proper error messages for user in case of synchronous workflow failure


``v4.2.16 Citrine``
===================

2018-03-02

Bug
---

* [EOS-2142] - eosfstregister fails to get mgm url in CentOS 7
* [EOS-2370] - EOSATLAS crashed while creating the output for a recursive attr set
* [EOS-2382] - FUSEX access with concurrency creates orphaned files
* [EOS-2386] - Vectored IO not accounted by "io" commands
* [EOS-2387] - FST crash in eos::fst::ReedSLayout::AddDataBlock

Task
----

* [EOS-2383] - eosxd: segfault in inval_inode

Improvement
-----------

* [EOS-1565] - RFE: turn off SIGSEGV handler on non-MGM EOS components


``v4.2.15 Citrine``
===================

2018-02-22

Bug
---

* [EOS-2353] - git clone with 2GB random reading creates read amplification
* [EOS-2359] - Deadlock in proto wfe
* [EOS-2361] - MGM crash after enabling ToggleDeadlock
* [EOS-2362] - eosfusebind (runuser) broken on slc6


``v4.2.14 Citrine``
===================

2018-02-20

Bug
----

* [EOS-2153] - consistent eosd memory leak
* [EOS-2348] - ns shows wrong value for resident memory (shows virtual)
* [EOS-2350] - eosd returns Numerical result out of range when talking to a CITRINE server and out of quota


``v4.2.13 Citrine``
===================

2018-02-19

Bug
----

* [EOS-2057] - Wrong conversion between IEC and Metric multiples
* [EOS-2299] - WFE can't be switched off
* [EOS-2309] - Possible memleak in FuseServer::Caps::BroadcastReleaseFromExternal
* [EOS-2310] - eosadmin wrapper no longer sends role
* [EOS-2330] - Usernames with 8 characters are wrongly mapped
* [EOS-2335] - Crash around XrdOucString::insert
* [EOS-2339] - "eos" shell crash around "eos_console_completion","eos_entry_generator"
* [EOS-2340] - "eos" crash around "AclHelper::CheckId"
* [EOS-2337] - autofs-ed fuse mounts not working for mountpoint names with matched entries under "/"

Task
----

* [EOS-2329] - protect MGM against memory exhaustion caused by a globbing ls

Improvement
-----------

* [EOS-2321] - Quota report TiB vs. TB
* [EOS-2323] - citrine mgm crash
* [EOS-2336] - Default smart files in the proc filesystem

Configuration Change
-------------------+

* [EOS-2279] - eosfusebind error message at login

Incident
--------

* [EOS-2298] - EOS MGM memory leak



``v4.2.12 Citrine``
===================

2018-02-01

Bug
---

* Fix deadlock observerd in EOSATLAS between gFsView.ViewMutex and pAddRmFsMutex from the
  scheduling part.
* Fix bug on the FST realted to the file id value going beyond 2^32-1
* [EOS-2275] - Possible data race in ThreadPool
* [EOS-2290] - increase shutdown timeout for the FSTs

New Feature
----------+

* Add skeleton for new "fs" command using protobuf requests
* Add skeleton for CTA integration
* Enhance the mutex deadlock detection mechanism


``v4.2.11 Citrine``
===================

2018-01-25

Bug
---

* [EOS-2264] - Fix possible insertion of an empty FS in FSView
* [EOS-2270] - FSCK crashed booting namespace
* [EOS-2271] - EOSPUBLIC deadlocked
* [EOS-2261] - "eos node ls <node>" with the monitoring flag does not apply the node filter
* [EOS-2267] - EOSPublic has crashed while recusively setting ACLs
* [EOS-2268] - Third party copying (on the same instance) fails with big files

Improvement
-----------

* [EOS-2283] - Double unlock in CITRINE code

Task
----

* [EOS-2244] - Understand EOSATLAS configuration issue


``v4.2.10 Citrine``
===================

2018-01-24

Bug
---

* [EOS-2264] Fix possible insertion of an empty FS in FSView
* [EOS-2258] If FST has qdb cluster configuration then to the dumpmd directly against QuarkDB
* [EOS-2277] fixes 'fake' truncation failing eu-strip in rpm builds of eos

Improvements
------------

* Refactoring of includes to speed up compilation, various build improvements
* avoid to call IsKnownNode to discover if an FST talks to the MGM, rely on sss + daemon user
* use (again) a reader-preferring mutex for the filesystem view


``v4.2.9 Citrine``
===================

2018-01-18

Bug
---

* [EOS-2228] Crash around forceRefreshSched related to pFsId2FsPtr

New Feature
-----------

* Filter out xrdcl.secuid/xrdcl.secgid tags on the FSTs to avoid triggering a
  bug on the xrootd client implementation

Improvements
------------

* [EOS-2253] Small writes should be aggregated with the journal
* Refactoring of the includes to speed up compilation


``v4.2.8 Citrine``
===================

2018-01-16

Bug
---

* [EOS-2184] - "eos ls -l" doesn't display the setgid bit anymore
* [EOS-2186] - eos ns reports wrong number of directory
* [EOS-2187] - Authproxy port only listens on IPv4
* [EOS-2211] - CITRINE deadlocks on namespace mutex
* [EOS-2216] - "binary junk" logged in func=RemoveGhostEntries (FID?)
* [EOS-2224] - selinux denials with eosfuse bind.
* [EOS-2229] - files downloaded with scp show 0 byte contents
* [EOS-2230] - read-ahead inefficiency
* [EOS-2231] - ioflush thread serializes file closeing and leads to memory aggregation
* [EOS-2241] - Directory TREE mv does not invalidate source caches

New Feature
-----------

* [EOS-2248] - FUSEX has to point ZMQ connection to active master

Improvement
-----------

* [EOS-2238] - Print a warning for 'node ...' functions when an FST is seen without a GEO tag

Support
-------
* [EOS-2208] - EOS MGM (new NS) aborts with "pure virtual method called" on update (restart?)


``v4.2.7 Citrine``
===================

2017-12-18

Bug
---

* [EOS-2207] - Work-around via environment variable to avoid loading too big no-replica sets (export EOS_NS_QDB_SKIP_UNLINKED_FILELIST)

* Many improvements and fixes for eosxd
  - fixing gateway mount options to work as NFS exports
  - fixing access function which was not refreshing caps/md objects

``v4.2.6 Citrine``
===================

2017-12-18

Bug
---

* [EOS-2150] - Repair option for d_mem_sz_diff error files
* [EOS-2202] - Lock-order-inversion between gAccessMutex and ViewMutex

* Many improvements and fixes for eosxd

``v4.2.5 Citrine``
===================

2017-12-12

Bug
---

* [EOS-2142] - eosfstregister fails to get mgm url in CentOS 7
* [EOS-2146] - symlinks have to show the size of the target string
* [EOS-2147] - listxattr creates SEGV on OSX
* [EOS-2148] - eosxd on OSX creates empty file when copying with 'cp'
* [EOS-2159] - An owner of a directory has to get always chmod permissions
* [EOS-2161] - rm -rf on fusex mount fails to remove all files/subdirectories
* [EOS-2167] - new file systems added go to 'spare.0'
* [EOS-2174] - Running out of FDs when using a user mount
* [EOS-2175] - eos ns command takes 10s on EOSPPS
* [EOS-2179] - calling verifychecksum issue
* [EOS-2180] - Unable to access quota space <filename> Read-only file system

* Many improvements and fixes for esoxd
* Performance improvements and fixes for the namespace and QuarkDB

``v4.2.4 Citrine``
===================

2017-11-28

Bug
----

* [EOS-2123] - eosxd renice's to lowest possible priority
* [EOS-2130] - segv while compiling eos
* [EOS-2137] - JSON output doesn't work anymore

Improvements
------------

* Many improvements and fixes for eosxd
* Many improvements and fixes for the namespace on QuarkDB


``v4.2.3 Citrine``
===================

2017-11-17

New features
------------

* New centralized draining implementation
* mgmofs.qdbcluster option in the configuration of the MGM to connect QuarkDB cluster

Improvements
------------

* Use the flusher also in the quota view of the new namespace
* Use prefetching for TPC transfers

Bug
---
* [EOS-2117] - mount.eosx should filter invalid options
* Fix ns stat statistics


``v4.2.2 Citrine``
===================

2017-11-14

Improvements
------------

* Many fixes for the eosxd fuse module
* Add eos_dump_proto_md tool to dump object metada info from QuarkDB
* Clean-up and improvements of the eos_ns_conversion tool for the new namespace
* Fix ns stat command not displaying ns info in monitoring format


``v4.2.1 Citrine``
===================

2017-11-10

Bug
---

* [EOS-2017] - MGM crash caused by FSCK
* [EOS-2061] - converter error in  "file adjustreplica" on raid6/archive layouts
* [EOS-2050] - Scheduling problem with adjustreplica and draining filesystem
* [EOS-2066] - xrdcp "Error [3005]" trying to transfer a "degraded" archive/raid6 file
* [EOS-2068] - Archive should use root identity when collecting files/dirs
* [EOS-2073] - MGM (citrine 4.1.30) unable to load configuration due to #iostat::udptargets with empty value
* [EOS-2092] - Auth proxy crashes
* [EOS-2093] - eos file convert from raid6/archive to replica:2 seems to not work.
* [EOS-2094] - JSON Return 0 instead of "NULL" when space.nominalsize is not defined

Task
----
* [EOS-1998] - Allow FST to login even when client traffic is stalled

Improvement
-----------

* [EOS-2101] - Report logical used-space when using xrootd commands
* A lot of improvements on the fusex side


``v4.2.0 Citrine``
===================

2017-10-23

Bug
----

* [EOS-1971] - EOS node listing crash
* [EOS-2015] - Table engine display values issue
* [EOS-2057] - Wrong conversion between IEC and Metric multiples
* [EOS-2060] - XrdMgmOfsFile SEGV out of bounds access

New Feature
-----------

* [EOS-2030] - Add '.' and '..' directories to file listings
* Prototype for the new fuse implementation i.e fusex
* Refactor of the ns command to use ProtoBuf-style commands

Task
----

* [EOS-2033] - quota id mapping for non-existing users

Bug
----

* [EOS-2016] - avoid SEGV when removing ghost entries on FST
* [EOS-2017] - avoid creating NULL object in map when resetting draining
* DOC: various corrections - use solar template with new WEB colour scheme


``v4.1.31 Citrine``
===================

2017-09-19

Bug
----

* [EOS-2016] - avoid SEGV when removing ghost entries on FST
* [EOS-2017] - avoid creating NULL object in map when resetting draining
* DOC: various corrections - use solar template with new WEB colour scheme

``v4.1.30 Citrine``
====================

2017-09-15

Bug
----
* [EOS-1978] - Preserve converted file ctime and ctime (CITRINE)
* FUSE: fix significant leak when returning a cached directory listing
* MGM: Enforce permission check when utime is executed
* MGM: Fix uid/gid overflow and comparison issues
* HTTP: fix ipv4/6 connection2ip function


``v4.1.29 Citrine``
===================

2017-09-08

Bug
----
* Mask the block checksum for draining and balancing when there is layout
  requesting blockchecksum for replica files.
* Add protection in case the proxys or the firewalleps vectors are not
  properly populated and we try to access a location beyond the size of the
  vector which leads to undefined behaviour.
* Multiple fixes to the Schedule2Drain code
* [EOS-1893] - EOS configuration can end up empty or truncated
* [EOS-1989] - eos file verify <path> -checksum is broken
* [EOS-1991] - eos-fuse rpm package broken dependency
* [EOS-1996] - space ls geo output is wrongly formatted

``v4.1.28 Citrine``
===================

2017-08-30

Bug
---
* [EOS-1991] - eos-fuse rpm package broken dependency

``v4.1.27 Citrine``
===================

2017-08-28

Bug
---
* [EOS-1976] - EOSD client memory leak
* [EOS-1986] - EOSPUBLIC: Crash when deleting a file entry
* [EOS-1984] - MGM: only show available fs on geosched show state latency and penalties tables.
* [EOS-1974] - NS: add missing initialization of pData (might lead to a SEGV during compaction if mmapping is disabled)

Improvement
-----------
* [EOS-1791] - RFE: attempt to auto-unmount on eos-fuse-core updates
* [EOS-1968] - eosd: always preload libjemalloc.so.1
* [EOS-1983] - Built-in http server should be dual-stack

New features
------------

* New accounting command - "eos accounting".

``v4.1.26 Citrine``
===================

2017-08-07

Bug
---
* [EOS-558] - "eos fileinfo" should better indicate non-active machines
* [EOS-1895] - MGM Crash when the groupscheduler can't place file
* [EOS-1897] - /var/log/eos/archive/eosarchived.log is world-writeable, should not
* [EOS-1906] - Incorrect GeoTree engine information
* [EOS-1936] - EOS ATLAS lost file due to balancing

Story
-----
* [EOS-1919] - Bug visible when creating YUM repositories on the FUSE mount in CITRINE instances

Improvement
------------
* [EOS-1159] - renaming a "quota node" directory gets rid of the quota setting?
* [EOS-1345] - documentation update - eos fs help
* [EOS-1875] - RFE: isolate eos client from LD_LIBRARY_PATH via RPATH

* Plus all the fixes from the 0.3.264 and 0.3.265 release form the bery_aquamarine branch.


``v4.1.25 Citrine``
===================

2017-06-29

Bugfix
------
* [EOS-542] - eos file version filename version modify the permissions of the file
* [EOS-1259] - MGM eos node ls display
* [EOS-1292] - "eos" hangs for 5min without EOS_MGM_URL - give verbose error message instead
* [EOS-1317] - command to drop/refresh UID / GID cache is not documented?
* [EOS-1762] - "eos attr link origin target" with a non-existent origin prevents listing of target's atrributes
* [EOS-1887] - Link back with the dynamic version of protobuf3
* [EOS-1889] - file verify command fails when specifyng fsid on a one-replica file
* [EOS-1893] - EOS configuration can end up empty or truncated
* [EOS-1888] - FSs wrongly reported as Unavailable by the GeoTreeEngine
* [EOS-1892] - File copy is scheduled on a full FS

New Feature
-----------
* [EOS-1872] - "Super" graceful FST shutdown
* There is a new dependency on protobuf3 packages both at build time and run time.
  These packages can be downloaded from the citrine-depend yum repository:
  http://storage-ci.web.cern.ch/storage-ci/eos/citrine-depend/el-7/x86_64/

Improvement
-----------
* [EOS-1581] - RFE: better error messages from the eos client, remove 'error: errc=0 msg=""'


``v4.1.24 Citrine``
===================

2017-06-14

Bugfix
------
* [EOS-162] - RFE: auto-refill spaces from "spare", up to "nominalsize"
* [EOS-455] - RFE: drop either fid: or fxid:, use the other consistently
* [EOS-1299] - MGM node and fs printout with long hostname
* [EOS-1716] - MGM: typo/missing whitespace in "client acting as directory owner" message
* [EOS-1859] - PPS crash while listing space
* [EOS-1877] - eos file drop does not accept fid:XXXX
* [EOS-1881] - List quota info not working anymore on EOSLHCB
* Fix fsck bug mixing information from different types of issues

Task
-----
* [EOS-1851] - mount.eos assumes sysv or systemd present

Improvement
-----------
* [EOS-1875] - RFE: isolate eos client from LD_LIBRARY_PATH via RPATH

Support
-------
* [EOS-1064] - get the year information for EOS file


``v4.1.23 Citrine``
===================

2017-05-17

Bugfix
------
* MGM: Take headroom into account when scheduling for placement
* MGM: Add protection in case the bookingsize is explicitly set to 0
* ARCHIVE: Use the MgmOfsAlias consistently otherwise the newly generated archive file will contain invalid JSON lines.


``v4.1.22 Citrine``
===================

2017-05-15

Bugfix
------
* Fix response for xrdfs query checksum to display "adler32" instead of "adler" as checksum type
* Fix launch of the follower thread for the MGM slave


``v4.1.21 Citrine``
===================

2017-05-12

Bugfix
------
* [EOS-1833] - eosfuse.cc uses a free'd fuse_req_t -> segfault
* [EOS-1781] - MGM crash in GeoBalancer
* [EOS-1642] - "Bad address" on EOS FUSE should be "Permission denied"
* [EOS-1830] - Recycle bin list crash when doing full scan (need protection)


Task
----
* [EOS-1848] - selinux error when uninstalling eos-fuse-core

User Documentation
------------------
* [EOS-1826] - Missing dependencies on the front page

Suggestion
----------
* [EOS-1827] - Ancient version of zmq.hpp causing issues when compiling with new zmq.h (taken from system)
* [EOS-1828] - Utils.hh in qclient #include cannot find header
* [EOS-1831] - CMAKE, microhttpd, and client
* [EOS-1832] - Bug in console/commands/com_fuse.cc with handling of environment variable EOS_FUSE_NO_MT


``v4.1.3 Citrine``
==================

2016-09-15

Bugfix
-------

* [EOS-1606] - Reading root files error when using eos 4.1.1
* [EOS-1609] - eos -b problem : *** Error in `/usr/bin/eos': free():


``v0.4.31 Citrine``
===================

2016-07-22

Bugfix
-------

- FUSE: when using krb5 or x509, allow both krb5/x509 and unix so that authentication
        does not fail on the fst (using only unix) when using XRootD >= 4.4


``v0.4.30 Citrine``
===================

2016-07-21

Bugfix
-------

- SPEC: Add workaround in the %posttrans section of the eos-fuse-core package
        to keep all the necessary files and directories when doing an update.
- CMAKE: Remove the /var/eos directory from the eos-fuse-core package and fix
        type in directory name.

``v0.4.29 Citrine``
===================

Bugfix
-------

- MGM: add monitoring switch to space,group status function
- MGM: draing mutex fix and fix double unlock when restarting a drain job
- MGM: fixes in JSON formatting, reencoding of non-http friendly tags/letters like <>?@
- FST: wait for pending async requests in the close method
- SPEC: remove directory creation scripting from spec files

New Features
------------

- RPM: build one source RPM which creates by default only client RPMs with less dependencies
