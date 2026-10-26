/*
 * s9io: 64-bit offset block I/O for TWRP (its BusyBox dd/shell use 32-bit
 * offsets and break past 4 GiB). Freestanding static aarch64 Linux binary,
 * no libc: build with build.sh.
 *
 *   s9io r DEV OFFSET LENGTH > out     read LENGTH bytes at byte OFFSET
 *   s9io w DEV OFFSET < in             write stdin at byte OFFSET, fsync
 *
 * Numbers are decimal or 0x hex. Exit status 0 on success.
 */
typedef unsigned long u64;
typedef long i64;

static i64
sys(i64 n, i64 a, i64 b, i64 c, i64 d)
{
	register i64 x8 __asm__("x8") = n;
	register i64 x0 __asm__("x0") = a;
	register i64 x1 __asm__("x1") = b;
	register i64 x2 __asm__("x2") = c;
	register i64 x3 __asm__("x3") = d;
	__asm__ volatile ("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

#define SYS_openat 56
#define SYS_close 57
#define SYS_lseek 62
#define SYS_read 63
#define SYS_write 64
#define SYS_fsync 82
#define SYS_exit 93
#define AT_FDCWD -100
#define O_RDONLY 0
#define O_WRONLY 1

static char buf[1 << 20];

static void
die(const char *m)
{
	u64 n = 0;
	while (m[n]) {
		n++;
	}
	sys(SYS_write, 2, (i64)m, (i64)n, 0);
	sys(SYS_exit, 1, 0, 0, 0);
}

static u64
num(const char *s)
{
	u64 v = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		for (s += 2; *s; s++) {
			int d = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : -1;
			if (d < 0) {
				die("s9io: bad number\n");
			}
			v = v * 16 + (u64)d;
		}
	} else {
		for (; *s; s++) {
			if (*s < '0' || *s > '9') {
				die("s9io: bad number\n");
			}
			v = v * 10 + (u64)(*s - '0');
		}
	}
	return v;
}

static void
put_all(i64 fd, const char *p, i64 n)
{
	while (n > 0) {
		i64 w = sys(SYS_write, fd, (i64)p, n, 0);
		if (w <= 0) {
			die("s9io: write failed\n");
		}
		p += w;
		n -= w;
	}
}

static int
run(int argc, char **argv)
{
	if (argc < 4 || (argv[1][0] != 'r' && argv[1][0] != 'w') || (argv[1][0] == 'r' && argc < 5)) {
		die("usage: s9io r DEV OFFSET LENGTH | s9io w DEV OFFSET < data\n");
	}
	int wr = argv[1][0] == 'w';
	i64 fd = sys(SYS_openat, AT_FDCWD, (i64)argv[2], wr ? O_WRONLY : O_RDONLY, 0);
	if (fd < 0) {
		die("s9io: open failed\n");
	}
	u64 off = num(argv[3]);
	if (sys(SYS_lseek, fd, (i64)off, 0, 0) != (i64)off) {
		die("s9io: seek failed\n");
	}
	if (!wr) {
		u64 left = num(argv[4]);
		while (left) {
			i64 n = sys(SYS_read, fd, (i64)buf, (i64)(left < sizeof(buf) ? left : sizeof(buf)), 0);
			if (n <= 0) {
				die("s9io: read failed\n");
			}
			put_all(1, buf, n);
			left -= (u64)n;
		}
	} else {
		for (;;) {
			i64 n = sys(SYS_read, 0, (i64)buf, sizeof(buf), 0);
			if (n < 0) {
				die("s9io: stdin read failed\n");
			}
			if (n == 0) {
				break;
			}
			put_all(fd, buf, n);
		}
		if (sys(SYS_fsync, fd, 0, 0, 0) != 0) {
			die("s9io: fsync failed\n");
		}
	}
	sys(SYS_close, fd, 0, 0, 0);
	return 0;
}

__attribute__((naked, noreturn)) void
_start(void)
{
	__asm__ volatile (
	    "ldr x0, [sp]\n"            /* argc */
	    "add x1, sp, #8\n"          /* argv */
	    "bl start_c\n");
}

__attribute__((noreturn, used)) void
start_c(int argc, char **argv)
{
	sys(SYS_exit, run(argc, argv), 0, 0, 0);
	for (;;) {
	}
}
