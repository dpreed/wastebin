/*
 * Command to waste CPUs and memory in Linux in such a way that a hypervisor
 * will emulate the system as if the system hardware were downsized
 * by a specified amount.
 * 
 * Copyright (c) 2021 David P. Reed
 *  
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute, 
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Theory of operation:
 *   The focus of this wastebin command is to force the Linux kernel to operate as if there
 *   were less underlying hardware available to the system, with strong realism. That is,
 *   a workload can be run beside this program to determine the optimal number of cpus
 *   and memory needed for a given performance, by "down-sizing" the effective system.
 *   
 *   wastebin captures memory pages by locking pages into memory using mlock(), and captures
 *   by taking the cpus offline (which requires that it be executed with root privileges,
 *   using sudo. To hold onto the pages, the program must not exit, essentially running
 *   as a daemon.
 *   While running as a daemon it can continue to adjust the number of cpus offline and
 *   the memory locked in RAM, receiving commands through a named pipe in the /tmp/wastebin
 *   file. When the wasted cpus and memory are both 0, the daemon terminates itself,
 *   deleting the named pipe. (the presence of the named pipe indicates that the daemon
 *   is running, so invocations of the command detect that and just send requests to the
 *   daemon rather than managing memory and cpu offlining directly. While the daemon is in
 *   operation, it waits for input on the named pipe, consuming no cpu resources, and
 *   and insignificant memory other than the locked DRAM blocks requested.
 *   The wasted memory pages are all zero (mlock creates zero pages), which a smart
 *   hypervisor (like the TidalScale hyperkernel) might optimize away, recreating them
 *   when accessed by the Linux kernel or application guest.
 * Usage:
 *   The command usage is shown by typing 'wastebin -h'. It takes two numeric arguments,
 *   the amount of memory (in various units of bytes, K bytes, M byte, G bytes, and T bytes)
 *   and the number of cpus that should be taken out of service by the program.
 *   It must be executed with root privileges to take cpus offline, via sudo.
 *   'wastebin 0 0' will restore the system to full operation, and terminate any daemon
 *   instance that may exist.
 */

#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

static void usage_exit(int ec, char *cmdstr)
{
	FILE *fh = (ec == EXIT_SUCCESS) ? stdout : stderr;
	fprintf(fh, "Usage:\n"
	       " %s -h\n"
	       " %s <mem> [<ncpus>]\n"
	       "  where <ncpus> is number of cpus to disable (default is 0) and\n"
	       "        <mem> is amount of memory to disable (required, may be 0).\n"
	       "              Suffix indicates units, case-insensitive, either K, M, G, T,\n"
	       "              for KiB, MiB, GiB, TiB\n",
		cmdstr, cmdstr);
	fflush(fh);
	exit(ec);
}

static void badarg_exit(char *argname, char *arg, char *cmdstr)
{
	fprintf(stderr, "%s: Bad %s argument '%s'\n", cmdstr, argname, arg);
	usage_exit(EXIT_FAILURE, cmdstr);
}

static void fail_exit(char *msg, char *cmdstr)
{
	fprintf(stderr, "%s: failed, %s\n", cmdstr, msg);
	fflush(stderr);
	exit(EXIT_FAILURE);
}

static long int str2ul(char *arg)
{
	char *endp;
	long int value = strtoul(arg, &endp, 10);
	if (*endp == '\0' && *arg != '\0' && value >= 0)
		return value;
	return -1;
}

static long int scale_ok(char mbz, long int value)
{
	return mbz ? -1L : ((value + 0xfff) & ~0xfffUL);
}

static long int strm2ul(char *arg)
{
	char *endp;
	long int value = strtoul(arg, &endp, 10);
	if (*arg != 0 && value >= 0)
		switch (endp[0]) {
		case '\0':
			return scale_ok(endp[0], value);
		case 'k':
		case 'K':
			return scale_ok(endp[1], value << 10);
		case 'm':
		case 'M':
			return scale_ok(endp[1], value << 20);
		case 'g':
		case 'G':
			return scale_ok(endp[1], value << 30);
		case 't':
		case 'T':
			return scale_ok(endp[1], value << 40);
	}
	return -1;
}

static ssize_t wastebin_max_size;     /* maximum size of memory that can be wasted */
static ssize_t wastebin_memory_taken = 0;
static char *wastebin_memory;	      /* segment that can hold enormous mem */

#define max_cpu_count 4096
static char cpus_online[max_cpu_count] = { 0 };
static char cpus_taken[max_cpu_count] = { 0 };
static unsigned wastebin_cpus_taken = 0;
static void set_cpu_online_state(unsigned cpu, char online_state)
{
	char sysfile[64];
	size_t nb;
	int fh;
	char digit;

	nb = snprintf(sysfile, sizeof(sysfile) - 1, "/sys/devices/system/cpu/cpu%u/online", cpu);
	sysfile[nb] = '\0';
	fh = open(sysfile, O_WRONLY);
	if (fh < 0) {
		perror("opening cpu online state file");
		fflush(stderr);
		exit(EXIT_FAILURE);
	}
	digit = "01"[(int)online_state];
	nb = write(fh, &digit, sizeof(digit));
	if (nb < sizeof(digit)) {
		perror("writing online state");
		fflush(stderr);
		exit(EXIT_FAILURE);
	}
	close(fh);
}

static void parse_sysfs_cpu_set(char *syscpuset, char *cpu_states)
{
	char sysfile[64];
	char buf[4096];
	size_t nb;
	int np;
	int nxt;
	int fh;
	nb = snprintf(sysfile, sizeof(sysfile) - 1, "/sys/devices/system/cpu/%s", syscpuset);
	sysfile[nb] = '\0';

	/* read sysfs file to get cpu set as string */
	fh = open(sysfile, O_RDONLY);
	if (fh < 0) return;
	nb = read(fh, &buf, sizeof(buf));
	close(fh);
	if (nb < 0 || nb == sizeof(buf))
		return;
	buf[nb] = '\0';

	/* parse scan over each comma separated group until newline or end of read */
	nxt = 0;
	do {
		unsigned u, ul;
		char c;
		int nd, end, nl, nle;

		np = sscanf(buf + nxt, "%u%n%1[,-]%n", &u, &nd, &c, &end);
		/* converts unsigned, checks for continuation by - or , */
		switch (np) {
		case EOF:
		case 0:
			/* invalid syntax encountered, just give up */
			fprintf(stderr, "invalid chars remaining after %d parsed, '%s'\n", nxt, buf + nxt);
			return;
		case 1:
			/* number not followed by valid continuation, nd contains bytes scanned  */
			if (u < max_cpu_count)
				cpu_states[u] = 1;
			nxt += nd;
			break;
		case 2:
			nxt += end;
			switch (c) {
			case '-':
				np = sscanf(buf + nxt, "%u%n%[,]%n", &ul, &nl, &c, &nle);
				switch (np) {
				case EOF:
				case 0:
					/* syntax error */
					fprintf(stderr, "invalid chars remaining after %d parsed, '%s'\n", nxt, buf + nxt);
					return;
				case 1:
					nxt += nl; /* if non-null chars here, it can't be digit, so we fail */
					break;
				case 2:
					nxt += nle;
					break;			
				}
				/* cpus from u:ul are present, comma not found */
				if (u < max_cpu_count && ul < max_cpu_count)
					for (unsigned i = u; i <= ul; i++)
						cpu_states[i] = 1;
				break;
			case ',':
				/* have eaten the comma here */
				if (u < max_cpu_count) cpu_states[u] = 1;
				break;
			}
		}
	} while(nxt != nb && buf[nxt] != '\n');
}

static unsigned count_cpu_set(char *cpu_states)
{
	unsigned count = 0;
	for (unsigned i = 0; i < max_cpu_count; i++)
		count += cpu_states[i];
	return count;
}

static void show_cpu_set(char *syscpuset, char *cpu_states)
{
	for (unsigned idx = 0; idx < max_cpu_count; idx++)
		if (cpu_states[idx])
			printf("CPU %u is %s\n", idx, syscpuset);
}

static void show_incore_memory(char *cmdstr)
{
	unsigned char ic_vec[4096];
	size_t step = sizeof(ic_vec) << 12;
	int ec;
	char *scanp;
	char *endp = wastebin_memory + wastebin_max_size;
	size_t pages = 0;
	for (scanp = wastebin_memory; scanp < endp; scanp += step) { 
		if (scanp + step > endp) /* partial last step */
			step = endp - scanp;
		ec = mincore(scanp, step, ic_vec);
		if (ec < 0)
			fprintf(stderr, "mincore failed @%lx, taken was %lx\n",
				scanp - wastebin_memory, wastebin_memory_taken);
		else {
			int npgs = step >> 12;
			for (int i = 0; i < npgs; i++)
				if (ic_vec[i] & 1) pages += 1;
		}
	}
	printf("%s: now wasting %'lu bytes out of %'lu\n", cmdstr, pages << 12,
	       wastebin_max_size);
	fflush(stdout);
}

static void inventory_cpus(void)
{
	parse_sysfs_cpu_set("online", cpus_online);
	show_cpu_set("online", cpus_online);
}

static void inventory_memory(char *cmdstr)
{
	wastebin_max_size = sysconf(_SC_PHYS_PAGES);
	if (wastebin_max_size < 0)
		fail_exit("getting physical memory size", cmdstr);
	wastebin_max_size <<= 12; /* convert pages to bytes */
	/* create anonymous private memory segment to waste memory */
	wastebin_memory = mmap(NULL, wastebin_max_size, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS |
			       MAP_NORESERVE | MAP_NONBLOCK,
			       -1, 0);
	if (wastebin_memory == MAP_FAILED) {
		fail_exit("getting waste memory segment", cmdstr);
	}
	wastebin_memory_taken = 0;
	/* disable huge pages in this region so MADV_REMOVE will actually
	 * succeed. Also disable samepage merging so pages are not merged */
	madvise(wastebin_memory, wastebin_max_size, MADV_NOHUGEPAGE);
	madvise(wastebin_memory, wastebin_max_size, MADV_UNMERGEABLE);

	show_incore_memory(cmdstr);
}

static void adjust_cpus(unsigned ncpus, char *cmdstr)
{
	unsigned n_taken;
	unsigned first_taken = 0;
	unsigned last_online = max_cpu_count;
	n_taken = count_cpu_set(cpus_taken);
	if (n_taken != wastebin_cpus_taken) {
		fprintf(stderr, "%s: %u cpus taken != %u expected\n",
			cmdstr, n_taken, wastebin_cpus_taken);
		fail_exit("adjusting cpus taken", cmdstr);			
	}
		
	if (ncpus != n_taken)
		printf("%s: adjust cpus taken to %u\n", cmdstr, ncpus);
	/* take some  online cpus offline */
	for (; n_taken < ncpus; n_taken++) {
		do {
			last_online -= 1;
		} while(last_online >= 0 && !cpus_online[last_online]);
		if (last_online == 0) /* serious problem */
			fail_exit("can't exhaust online cpus", cmdstr);
		cpus_online[last_online] = 0;
		cpus_taken[last_online] = 1;
		set_cpu_online_state(last_online, 0);
	}
	/* put some taken cpus back online */
	for (; n_taken > ncpus; n_taken--) {
		while(!cpus_taken[first_taken]) {
			first_taken += 1;
		}
		cpus_taken[first_taken] = 0;
		cpus_online[first_taken] = 1;
		set_cpu_online_state(first_taken, 1);
		first_taken += 1;
	}
	wastebin_cpus_taken = ncpus;
	show_cpu_set("taken", cpus_taken);
}

static void adjust_memory(long membytes, char *cmdstr)
{
	if (wastebin_memory_taken < membytes) {
		fprintf(stderr, "%s: mlock called to lock %'lu bytes\n",
			cmdstr, membytes - wastebin_memory_taken);
		/* mlock sets all pages to zeros */
		int ec = mlock(wastebin_memory + wastebin_memory_taken,
		      membytes - wastebin_memory_taken);
		fprintf(stderr, "%s: has locked %'lu bytes\n",
			cmdstr, membytes - wastebin_memory_taken);
		fflush(stderr);
		switch (ec) {
		case 0:
			break;
		case EPERM:
			fail_exit("Lock memory requires CAP_IPC_LOCK", cmdstr);
		default:
			fail_exit("Can't lock the pages in memory\n", cmdstr);
		}
		wastebin_memory_taken = membytes;
	} else if (wastebin_memory_taken > membytes) {
		munlock(wastebin_memory + membytes,
			wastebin_memory_taken - membytes);
		madvise(wastebin_memory + membytes,
			wastebin_memory_taken - membytes, MADV_DONTNEED);
		wastebin_memory_taken = membytes;
	}
        show_incore_memory(cmdstr);
}

int main(int argc, char *argv[])
{
	char *cmdstr = argv[0];
	char *memarg = "";;
	char *cpuarg = "0";
	int pid;
	int logid;
	FILE *logf;
	struct {		/* can be sent as a message if server detected */
		long membytes, cpus;
	} desired;
	struct pollfd pipe_poll;
	int pipefd;
	int ec;
	ssize_t nb;

	setlocale(LC_ALL, "");

	if (argc < 2 || 0 == strcmp("-h", argv[1]))
		usage_exit(EXIT_SUCCESS, cmdstr);
	memarg = argv[1];
	if (argc > 2) cpuarg = argv[2];
	if (argc > 3)
		usage_exit(EXIT_FAILURE, cmdstr);
	
	desired.cpus = str2ul(cpuarg);
	if (desired.cpus < 0)
		badarg_exit("cpus", cpuarg, cmdstr);
	desired.membytes = strm2ul(memarg);
	if (desired.membytes < 0)
		badarg_exit("memory", memarg, cmdstr);

	/* create named pipe to adjust wastebin size */
	ec = mkfifo("/tmp/wastebin", S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP);
	if (ec < 0)
		switch (errno) {
		default:
			fail_exit("Getting pipe", cmdstr);
		case EEXIST:
			/* use existing wastebin process */
			pipefd = open("/tmp/wastebin", O_WRONLY | O_NONBLOCK);
			if (pipefd >= 0) {
				/* send command args to wastebin process */
				nb = write(pipefd, &desired, sizeof(desired)); 
				close(pipefd);
				return 0;
			}
			fprintf(stderr, "%s: reusing existing /tmp/wastebin\n", cmdstr);
		}
	
	/* after this point, become a "server" to hold onto memory, by daemonizing ourself */

	/* open a log file to handle server output */
	logid = open("/tmp/wastebin.log", O_WRONLY | O_APPEND | O_CREAT, S_IRWXU | S_IRWXG | S_IROTH);
	if (logid < 0) {
		fprintf(stderr, "%s: Can't create log file /tmp/wastebin.log\n", cmdstr);
		return EXIT_FAILURE;
	}
	logf = fdopen(logid, "w"); /* also create a stream on logid for fprintf's below */
	if (logf == NULL) {
		fprintf(stderr, "%s: Can't create log file /tmp/wastebin.log\n", cmdstr);
		return EXIT_FAILURE;
	}
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "%s: unable to fork\n", cmdstr);
		return EXIT_FAILURE;
	}
	if (pid > 0) {
		printf("%s: Forked off a background process to acquire and hold memory\n", cmdstr);
		return EXIT_SUCCESS;
	}
	/* now in child, adjust file descriptors in background process to direct output to log file */
	close(STDIN_FILENO);
	ec = dup2(logid, STDERR_FILENO);
	if (ec < 0) {
		fprintf(logf, "%s: couldn't redirect stderr to log\n", cmdstr);
		return EXIT_FAILURE;
	}
	ec = dup2(logid, STDOUT_FILENO);
	if (ec < 0) {
		fprintf(logf, "%s: couldn't redirect stdout to log\n", cmdstr);
		return EXIT_FAILURE;
	}
	/* detach this process from the parent's session */
	if (setsid() < 0) {
		fprintf(stderr, "%s: couldn't remove background process from session\n", cmdstr);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "%s: Background process started.\n", cmdstr);
	
	/* open listening pipe. Bu opening for read *and* write, a client
	 * closing its end of the pipe won't cause an EOF */
	pipefd = open("/tmp/wastebin", O_RDWR | O_NONBLOCK);
	if (pipefd < 0) {
		fprintf(stderr, "%s: could not open /tmp/wastebin to read\n", cmdstr);
		return EXIT_FAILURE;
	}
	pipe_poll.fd = pipefd;
	pipe_poll.events = POLLIN;
	pipe_poll.revents = 0;

	printf("%s: Inventorying currently online cpus and memory\n", cmdstr);
	inventory_cpus();
	inventory_memory(cmdstr);


	while(1) {
		printf("%s: disabling %ld cpus and %'ld bytes of memory\n",
		       cmdstr, desired.cpus, desired.membytes);
		adjust_cpus(desired.cpus, cmdstr);
		adjust_memory(desired.membytes, cmdstr);

		/* don't remain a server if the wastebin is now empty of cpus and memory */
		if (wastebin_cpus_taken == 0 && wastebin_memory_taken == 0)
			break;
		
		/* get/wait for next client message to change wastebin size */
		while((nb = read(pipefd, &desired, sizeof(desired)))
		      != sizeof(desired)) {
			if (nb > 0 || (nb < 0 && errno != EAGAIN)) {
				perror("debug");
				printf("nb %ld errno %d\n", nb, errno);
				fail_exit("reading client command", cmdstr);
			}
			do {
				ec = poll(&pipe_poll, 1, 50000);
				if (ec < 0)
					fail_exit("polling named pipe", cmdstr);
			} while ((pipe_poll.revents & POLLIN) == 0);
			pipe_poll.revents = 0;
		}
	}

	close(pipefd);
	ec = unlink("/tmp/wastebin");
	if (ec < 0) {
		fprintf(stderr, "%s: can't remove named pipe /tmp/wastebin\n", cmdstr);
		return EXIT_FAILURE;
	}
	fprintf(stderr, "%s: background process terminated\n", cmdstr);
	fflush(stderr);
	close(logid);
	return EXIT_SUCCESS;
}
