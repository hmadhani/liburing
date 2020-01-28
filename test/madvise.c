/*
 * Description: basic madvise test
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>

#include "liburing.h"

#define FILE_SIZE	(128 * 1024)

#define LOOPS		100
#define MIN_LOOPS	10

static unsigned long long utime_since(const struct timeval *s,
				      const struct timeval *e)
{
	long long sec, usec;

	sec = e->tv_sec - s->tv_sec;
	usec = (e->tv_usec - s->tv_usec);
	if (sec > 0 && usec < 0) {
		sec--;
		usec += 1000000;
	}

	sec *= 1000000;
	return sec + usec;
}

static unsigned long long utime_since_now(struct timeval *tv)
{
	struct timeval end;

	gettimeofday(&end, NULL);
	return utime_since(tv, &end);
}

static int create_file(const char *file)
{
	ssize_t ret;
	char *buf;
	int fd;

	buf = malloc(FILE_SIZE);
	memset(buf, 0xaa, FILE_SIZE);

	fd = open(file, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		perror("open file");
		return 1;
	}
	ret = write(fd, buf, FILE_SIZE);
	fsync(fd);
	close(fd);
	return ret != FILE_SIZE;
}

static int do_madvise(struct io_uring *ring, void *addr, off_t len, int advice)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		fprintf(stderr, "failed to get sqe\n");
		return 1;
	}

	io_uring_prep_madvise(sqe, addr, len, advice);
	sqe->user_data = advice;
	ret = io_uring_submit_and_wait(ring, 1);
	if (ret != 1) {
		fprintf(stderr, "submit: %d\n", ret);
		return 1;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret) {
		fprintf(stderr, "wait: %d\n", ret);
		return 1;
	}

	ret = cqe->res;
	if (ret == -EINVAL || ret == -EBADF) {
		fprintf(stdout, "Madvise not supported, skipping\n");
		exit(0);
	} else if (ret) {
		fprintf(stderr, "cqe->res=%d\n", cqe->res);
	}
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

static long do_copy(int fd, char *buf, void *ptr)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	memcpy(buf, ptr, FILE_SIZE);
	return utime_since_now(&tv);
}

static int test_madvise(struct io_uring *ring, const char *filename)
{
	unsigned long cached_read, uncached_read, cached_read2;
	int fd, ret;
	char *buf;
	void *ptr;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	buf = malloc(FILE_SIZE);

	ptr = mmap(NULL, FILE_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
	if (ptr == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	cached_read = do_copy(fd, buf, ptr);
	if (cached_read == -1)
		return 1;

	cached_read = do_copy(fd, buf, ptr);
	if (cached_read == -1)
		return 1;

	ret = do_madvise(ring, ptr, FILE_SIZE, MADV_DONTNEED);
	if (ret)
		return 1;

	uncached_read = do_copy(fd, buf, ptr);
	if (uncached_read == -1)
		return 1;

	ret = do_madvise(ring, ptr, FILE_SIZE, MADV_DONTNEED);
	if (ret)
		return 1;

	ret = do_madvise(ring, ptr, FILE_SIZE, MADV_WILLNEED);
	if (ret)
		return 1;

	msync(ptr, FILE_SIZE, MS_SYNC);

	cached_read2 = do_copy(fd, buf, ptr);
	if (cached_read2 == -1)
		return 1;

	if (cached_read < uncached_read &&
	    cached_read2 < uncached_read)
		return 0;

	return 2;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	int ret, i, good, bad;

	if (create_file(".madvise.tmp")) {
		fprintf(stderr, "file creation failed\n");
		goto err;
	}
	if (io_uring_queue_init(8, &ring, 0)) {
		fprintf(stderr, "ring creation failed\n");
		goto err;
	}

	good = bad = 0;
	for (i = 0; i < LOOPS; i++) {
		ret = test_madvise(&ring, ".madvise.tmp");
		if (ret == 1) {
			fprintf(stderr, "test_madvise failed\n");
			goto err;
		} else if (!ret)
			good++;
		else if (ret == 2)
			bad++;
		if (i >= MIN_LOOPS && !bad)
			break;
	}

	if (bad > good)
		fprintf(stderr, "Suspicious timings (%u > %u)\n", bad, good);
	unlink(".madvise.tmp");
	io_uring_queue_exit(&ring);
	return 0;
err:
	unlink(".madvise.tmp");
	return 1;
}