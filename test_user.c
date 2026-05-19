/* test_user.c
 *
 * Userspace test program for /dev/dummydriver.
 *
 * Sequence:
 *   1. open()   the device
 *   2. write()  a string into the driver buffer
 *   3. lseek()  back to offset 0 so read() starts from the beginning
 *   4. read()   the data back out
 *   5. verify   the round-trip (strcmp)
 *   6. close()  the device
 *
 * Cross-compile (ARM target):
 *   arm-none-linux-gnueabihf-gcc -Wall -Wextra -o test_user test_user.c
 *
 * Run on the target board:
 *   ./test_user
 *   dmesg | tail -20     # inspect kernel-side printk messages
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>      /* open(), O_RDWR */
#include <unistd.h>     /* close(), read(), write(), lseek() */
#include <errno.h>      /* errno */

#define DEVICE    "/dev/dummydriver"
#define BUF_SIZE  256

/* ── helper: print an errno-annotated error message then exit ─────────────── */
static void die(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

int main(void)
{
	int     fd;
	char    write_buf[] = "Hello from userspace!";
	char    read_buf[BUF_SIZE];
	ssize_t n;

	printf("=== dummydriver userspace test ===\n\n");

	/* ── 1. Open the device ───────────────────────────────────────────── */
	fd = open(DEVICE, O_RDWR);
	if (fd < 0)
		die("open " DEVICE);
	printf("[OK] opened %s (fd=%d)\n", DEVICE, fd);

	/* ── 2. Write to the driver buffer ───────────────────────────────── */
	n = write(fd, write_buf, strlen(write_buf));
	if (n < 0)
		die("write");
	if ((size_t)n != strlen(write_buf)) {
		fprintf(stderr, "[WARN] short write: requested %zu, wrote %zd\n",
		        strlen(write_buf), n);
	}
	printf("[OK] wrote   %zd byte(s): \"%s\"\n", n, write_buf);

	/* ── 3. Seek back to the beginning ───────────────────────────────── */
	/*
	 * The file offset was advanced by write().  We must reset it so the
	 * following read() retrieves data from offset 0, not from the end of
	 * what we just wrote (which would return 0 bytes / EOF).
	 */
	if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
		die("lseek");
	printf("[OK] seeked  back to offset 0\n");

	/* ── 4. Read back from the driver buffer ─────────────────────────── */
	memset(read_buf, 0, sizeof(read_buf));
	n = read(fd, read_buf, sizeof(read_buf) - 1);
	if (n < 0)
		die("read");
	printf("[OK] read    %zd byte(s): \"%s\"\n", n, read_buf);

	/* ── 5. Verify round-trip ────────────────────────────────────────── */
	if (strcmp(write_buf, read_buf) == 0)
		printf("\n[PASS] Round-trip write→read successful!\n");
	else
		printf("\n[FAIL] Mismatch:\n"
		       "       wrote: \"%s\"\n"
		       "       read : \"%s\"\n", write_buf, read_buf);

	/* ── 6. Close the device ─────────────────────────────────────────── */
	if (close(fd) < 0)
		die("close");
	printf("[OK] closed  %s\n", DEVICE);

	return EXIT_SUCCESS;
}
