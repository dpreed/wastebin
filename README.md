Tool to take active cpus and memory pages offline, to reduce the capacity of
a running linux system. Does not require modifications to the Linux kernel.

The command requires that the user have the IPC_LOCK capability
because it takes memory out of service by locking pages into RAM and forking off
a background process. If run as root, IPC_LOCK is present.

It also requires running as root (via sudo) to take cpus offline and online.

More detailed documentation is present in the C source code.
