#!/usr/bin/env python3
"""Exercise the generated adapter's actual sync-file functions on the host."""
import pathlib, subprocess, sys, tempfile
source = pathlib.Path(sys.argv[1]).read_text()
start = source.index('int\ndrmSyncobjExportSyncFile(')
end = source.index('int\ndrmSyncobjHandleToFD(', start)
preamble = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#define FD_SYNCFILE 1
struct ssync { uint32_t seq; int signaled; };
struct sfd { uint32_t value; };
static struct ssync obj;
static struct sfd imported = {42};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct ssync *ssync(uint32_t h) { return h == 1 ? &obj : 0; }
static struct sfd *sfd(int fd, int kind) { (void)kind; return fd == 12 ? &imported : 0; }
static int new_fd(int kind, uint32_t seq) { (void)kind; return 100 + seq; }
'''
tests = r'''
int main(void) {
 int fd = -99;
 assert(drmSyncobjExportSyncFile(0, 1, &fd) == -EINVAL && fd == -99);
 assert(drmSyncobjImportSyncFile(0, 1, 1234) == -EINVAL && !obj.signaled);
 assert(drmSyncobjImportSyncFile(0, 1, -1) == 0 && obj.signaled);
 assert(drmSyncobjExportSyncFile(0, 1, &fd) == 0 && fd == 100);
 assert(drmSyncobjImportSyncFile(0, 1, 12) == 0 && obj.seq == 42 && !obj.signaled);
 assert(drmSyncobjImportSyncFile(0, 1, 1234) == -EINVAL && obj.seq == 42);
 assert(drmSyncobjExportSyncFile(0, 1, &fd) == 0 && fd == 142);
 assert(drmSyncobjImportSyncFile(0, 9, -1) == -EINVAL);
 assert(drmSyncobjExportSyncFile(0, 9, &fd) == -EINVAL);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 c = pathlib.Path(tmp)/'test.c'; exe = pathlib.Path(tmp)/'test'
 c.write_text(preamble + source[start:end] + tests)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-pthread',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: empty fence export, invalid fd import, completed sentinel, pending fence import/export, invalid handle')
