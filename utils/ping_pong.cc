/*
   A ping-pong fcntl byte range lock test

   Copyright 2002      Andrew Tridgell

   Copyright 2015 Skytechnology sp. z o.o.
   Copyright 2023 Leil Storage OÜ

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

/*
  This measures the ping-pong byte range lock latency. It is
  especially useful on a cluster of nodes sharing a common lock
  manager as it will give some indication of the lock managers
  performance under stress.

  tridge@samba.org, February 2002
*/

#define _XOPEN_SOURCE 500

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

static struct timeval tp1,tp2;

static int do_reads, do_writes, use_mmap;

static const int kLoopLimit = 10e3;

static void start_timer(void)
{
	gettimeofday(&tp1,NULL);
}

static double end_timer(void)
{
	gettimeofday(&tp2,NULL);
	return (tp2.tv_sec + (tp2.tv_usec*1.0e-6)) -
		(tp1.tv_sec + (tp1.tv_usec*1.0e-6));
}

/* lock a byte range in a open file */
static int lock_range(int fd, int offset, int len)
{
	struct flock lock;

	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = offset;
	lock.l_len = len;
	lock.l_pid = 0;

	return fcntl(fd,F_SETLKW,&lock);
}

/* unlock a byte range in a open file */
static int unlock_range(int fd, int offset, int len)
{
	struct flock lock;

	lock.l_type = F_UNLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = offset;
	lock.l_len = len;
	lock.l_pid = 0;

	return fcntl(fd,F_SETLKW,&lock);
}

/* run the ping pong test on fd */
static void ping_pong(int fd, int num_locks)
{
	unsigned count = 0;
	int i=0, loops=0;
	unsigned char *val;
	unsigned char incr=0, last_incr=0;
	unsigned char *p = NULL;
	int ret;

	ret = ftruncate(fd, num_locks+1);
	if (ret == -1) {
		printf("ftruncate failed: %s\n", strerror(errno));
		return;
	}

	if (use_mmap) {
		p = (unsigned char *)mmap(NULL, num_locks+1, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			printf("mmap failed: %s\n", strerror(errno));
			return;
		}
	}

	val = (unsigned char *)calloc(num_locks+1, sizeof(unsigned char));
	if (val == NULL) {
		printf("calloc failed\n");
		return;
	}

	start_timer();

	lock_range(fd, 0, 1);
	i = 0;

	while (loops < kLoopLimit) {
		if (lock_range(fd, (i+1) % num_locks, 1) != 0) {
			printf("lock at %d failed! - %s\n",
			       (i+1) % num_locks, strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (do_reads) {
			unsigned char c;
			if (use_mmap) {
				c = p[i];
			} else if (pread(fd, &c, 1, i) != 1) {
				printf("read failed at %d\n", i);
				exit(EXIT_FAILURE);
			}
			incr = c - val[i];
			val[i] = c;
		}
		if (do_writes) {
			char c = val[i] + 1;
			if (use_mmap) {
				p[i] = c;
			} else if (pwrite(fd, &c, 1, i) != 1) {
				printf("write failed at %d\n", i);
				exit(EXIT_FAILURE);
			}
		}
		if (unlock_range(fd, i, 1) != 0) {
			printf("unlock at %d failed! - %s\n",
			       i, strerror(errno));
		}
		i = (i+1) % num_locks;
		count++;
		if (loops > num_locks && incr != last_incr) {
			last_incr = incr;
			printf("data increment = %u\n", incr);
			fflush(stdout);
		}
		if (end_timer() > 1.0) {
			printf("%8u locks/sec\r",
			       (unsigned)(2*count/end_timer()));
			fflush(stdout);
			start_timer();
			count=0;
		}
		loops++;
	}

	if (unlock_range(fd, i, 1) != 0) {
		printf("unlock at %d failed! - %s\n",
		       i, strerror(errno));
		exit(EXIT_FAILURE);
	}
	free(val);
}

int main(int argc, char *argv[])
{
	char *fname;
	int fd, num_locks;
	int c;

	while ((c = getopt(argc, argv, "rwm")) != -1) {
		switch (c){
		case 'w':
			do_writes = 1;
			break;
		case 'r':
			do_reads = 1;
			break;
		case 'm':
			use_mmap = 1;
			break;
		default:
			fprintf(stderr, "Unknown option '%c'\n", c);
			exit(1);
		}
	}

	argv += optind;
	argc -= optind;

	if (argc < 2) {
		printf("ping_pong [options] <file> <num_locks>\n");
		printf("           -r    do reads\n");
		printf("           -w    do writes\n");
		printf("           -m    use mmap\n");
		exit(1);
	}

	fname = argv[0];
	num_locks = atoi(argv[1]);

	fd = open(fname, O_CREAT|O_RDWR, 0600);
	if (fd == -1) exit(EXIT_FAILURE);

	ping_pong(fd, num_locks);

	return EXIT_SUCCESS;
}
