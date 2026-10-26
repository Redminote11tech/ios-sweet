/*
 * SWEETGL_OSLOG=1: copy the process's own os_log messages (default, info,
 * error, fault; not debug) into the kernel log through sweet_drm_log, so
 * logd-only diagnostics (PosterBoard, SpringBoard...) show in sweetlog.
 *
 * _os_log_impl/_os_log_error_impl/_os_log_fault_impl are interposed
 * (we are a DYLD_INSERT_LIBRARIES image). The encoded argument buffer is
 * decoded here: buf[0] summary, buf[1] count, then per argument a
 * descriptor byte (type << 4 | flags), a size byte and the data.
 * Without SWEETGL_OSLOG the hooks only forward.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sweet_drm.h"

typedef void *os_log_ref;
extern void _os_log_impl(void *dso, os_log_ref log, uint8_t type, const char *format, uint8_t *buf, uint32_t size);
extern void _os_log_error_impl(void *dso, os_log_ref log, uint8_t type, const char *format, uint8_t *buf, uint32_t size);
extern void _os_log_fault_impl(void *dso, os_log_ref log, uint8_t type, const char *format, uint8_t *buf, uint32_t size);

int sgl_oslog_on;
static unsigned sgl_oslog_lines;

struct arg {
	uint8_t type, size;
	const uint8_t *data;
};

static uint64_t
arg_u(const struct arg *a)
{
	uint64_t v = 0;
	if (a && a->size <= 8) {
		memcpy(&v, a->data, a->size);
	}
	return v;
}

static void
put(char *out, size_t *o, size_t cap, const char *s)
{
	while (*s && *o + 1 < cap) {
		out[(*o)++] = *s++;
	}
	out[*o] = 0;
}

static void
render(const char *fmt, const uint8_t *buf, uint32_t size, char *out, size_t cap)
{
	struct arg args[32];
	int n = 0;
	if (buf != NULL && size >= 2) {
		uint32_t p = 2;
		for (int i = 0; i < buf[1] && i < 32 && p + 2 <= size; i++) {
			args[n].type = buf[p] >> 4;
			args[n].size = buf[p + 1];
			args[n].data = buf + p + 2;
			if (p + 2 + args[n].size > size) {
				break;
			}
			p += 2 + args[n].size;
			n++;
		}
	}
	int ai = 0;
	size_t o = 0;
	out[0] = 0;
	for (const char *f = fmt; *f && o + 1 < cap;) {
		if (*f != '%') {
			out[o++] = *f++;
			out[o] = 0;
			continue;
		}
		if (f[1] == '%') {
			put(out, &o, cap, "%");
			f += 2;
			continue;
		}
		const char *start = f++;
		if (*f == '{') {
			while (*f && *f != '}') {
				f++;
			}
			if (*f) {
				f++;
			}
		}
		int prec = -1;
		while (*f && strchr("-+ #0123456789.*hlqjztL", *f)) {
			if (*f == '*') {
				prec = (int)arg_u(ai < n ? &args[ai] : NULL);
				ai++;
			}
			f++;
		}
		char conv = *f ? *f++ : 0;
		const struct arg *a = ai < n ? &args[ai++] : NULL;
		char tmp[160];
		tmp[0] = 0;
		if (a == NULL) {
			snprintf(tmp, sizeof(tmp), "<%.*s?>", (int)(f - start), start);
		} else if (a->type == 2 && (conv == 's' || conv == '@')) {      /* string pointer */
			const char *s = (const char *)(uintptr_t)arg_u(a);
			snprintf(tmp, sizeof(tmp), "%.*s", prec >= 0 && prec < 150 ? prec : 150, s ? s : "(null)");
		} else if (a->type == 4 || conv == '@') {                      /* object */
			CFTypeRef obj = (CFTypeRef)(uintptr_t)arg_u(a);
			CFStringRef d = obj ? CFCopyDescription(obj) : NULL;
			if (d == NULL || !CFStringGetCString(d, tmp, sizeof(tmp), kCFStringEncodingUTF8)) {
				snprintf(tmp, sizeof(tmp), obj ? "<obj %p>" : "(null)", obj);
			}
			if (d) {
				CFRelease(d);
			}
		} else if (a->type == 1 || a->type == 3) {                      /* count/data: skip */
			snprintf(tmp, sizeof(tmp), "<data %u>", a->size);
		} else if (conv == 'f' || conv == 'g' || conv == 'e') {
			double dv = 0;
			if (a->size == 8) {
				memcpy(&dv, a->data, 8);
			}
			snprintf(tmp, sizeof(tmp), "%g", dv);
		} else if (conv == 'x' || conv == 'X' || conv == 'p') {
			snprintf(tmp, sizeof(tmp), "%#llx", (unsigned long long)arg_u(a));
		} else if (conv == 'u') {
			snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)arg_u(a));
		} else if (conv == 'c') {
			snprintf(tmp, sizeof(tmp), "%c", (char)arg_u(a));
		} else {
			int64_t v = (int64_t)arg_u(a);
			if (a->size == 4) {
				v = (int32_t)v;
			}
			snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
		}
		put(out, &o, cap, tmp);
	}
}

static void
emit(uint8_t type, const char *fmt, uint8_t *buf, uint32_t size)
{
	static __thread int busy;
	if (!sgl_oslog_on || busy || fmt == NULL || type == 2 /* debug */ || sgl_oslog_lines >= 4000) {
		return;
	}
	busy = 1;
	sgl_oslog_lines++;
	char line[440];
	render(fmt, buf, size, line, sizeof(line));
	const char *t = type == 0x10 ? "E" : type == 0x11 ? "F" : type == 1 ? "I" : "N";
	sweet_drm_log("oslog %s: %s", t, line);
	busy = 0;
}

static void
hook_impl(void *dso, os_log_ref log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
	emit(type, format, buf, size);
	_os_log_impl(dso, log, type, format, buf, size);
}

static void
hook_error(void *dso, os_log_ref log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
	emit(0x10, format, buf, size);
	_os_log_error_impl(dso, log, type, format, buf, size);
}

static void
hook_fault(void *dso, os_log_ref log, uint8_t type, const char *format, uint8_t *buf, uint32_t size)
{
	emit(0x11, format, buf, size);
	_os_log_fault_impl(dso, log, type, format, buf, size);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} sgl_interpose[] = {
	{(const void *)hook_impl, (const void *)_os_log_impl},
	{(const void *)hook_error, (const void *)_os_log_error_impl},
	{(const void *)hook_fault, (const void *)_os_log_fault_impl},
};
