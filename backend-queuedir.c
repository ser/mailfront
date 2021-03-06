#include "mailfront.h"

#include <sysdeps.h>
#include <systime.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include <iobuf/iobuf.h>
#include <path/path.h>
#include <str/str.h>

static str destpath;
static str temppath;
static str destname;
static str tempname;
static int tmpfd;
static obuf msgbuf;

static RESPONSE(createerr, 451, "4.3.0 Error creating queue file");
static RESPONSE(writeerr, 451, "4.3.0 Error writing queue file");
static RESPONSE(configerr, 451, "4.3.0 Missing backend configuration parameter");

static const response* reset(void)
{
  if (tempname.len)
    unlink(tempname.s);
  if (destname.len)
    unlink(destname.s);
  tempname.len = destname.len = 0;
  return 0;
}

static const response* make_filenames(void)
{
  pid_t pid = getpid();
  for (;;) {
    struct timeval tv;
    struct stat st;

    gettimeofday(&tv, 0);

    if (!str_copyf(&tempname, "S{/}d{.}06d{.}d", &temppath, tv.tv_sec, tv.tv_usec, pid))
      return &resp_oom;
    if (lstat(tempname.s, &st) == 0) continue;
    if (errno != ENOENT) return &resp_internal;

    if (!str_copyf(&destname, "S{/}d{.}06d{.}d", &destpath, tv.tv_sec, tv.tv_usec, pid))
      return &resp_oom;
    if (lstat(destname.s, &st) != 0) {
      if (errno != ENOENT) return &resp_internal;
      return 0;
    }
    sleep(1);
  }
}

static const response* sender(str* address, str* params)
{
  const response* r;
  const char* destdir = session_getenv("QUEUEDIR");
  const char* tempsubdir = session_getenv("QUEUEDIR_TMP");
  const char* destsubdir = session_getenv("QUEUEDIR_DEST");

  if (destdir == 0)
    return &resp_configerr;
  if (tempsubdir == 0)
    tempsubdir = "tmp";
  if (destsubdir == 0)
    destsubdir = "new";
  if (!str_copyf(&destpath, "s{/}s", destdir, destsubdir)
      || !str_copyf(&temppath, "s{/}s", destdir, tempsubdir))
    return &resp_oom;
  if ((r = make_filenames()) != 0)
    return r;
  obuf_close(&msgbuf);
  if (!obuf_open(&msgbuf, tempname.s, OBUF_CREATE | OBUF_EXCLUSIVE, 0666, 0))
    return &resp_createerr;
  if (!obuf_putstr(&msgbuf, address) || !obuf_putc(&msgbuf, 0)) {
    reset();
    return &resp_writeerr;
  }
  return 0;
  (void)params;
}

static const response* recipient(str* address, str* params)
{
  if (!obuf_putstr(&msgbuf, address) || !obuf_putc(&msgbuf, 0))
    return &resp_writeerr;
  return 0;
  (void)params;
}

static const response* data_start(int fd)
{
  /* Sender hasn't been sent, so save the message to a temporary file. */
  if (destname.len == 0) {
    if ((tmpfd = scratchfile()) < 0)
      return &resp_writeerr;
  }
  else {
    tmpfd = 0;
    if (!obuf_putc(&msgbuf, 0))
      return &resp_writeerr;
  }
  return 0;
  (void)fd;
}

static const response* data_block(const char* bytes, unsigned long len)
{
  if (tmpfd > 0) {
    if ((unsigned long)write(tmpfd, bytes, len) != len)
      return &resp_writeerr;
  }
  else
    if (!obuf_write(&msgbuf, bytes, len))
      return &resp_writeerr;
  return 0;
}

static const response* message_end(int fd)
{
  int dosync = session_getenv("QUEUEDIR_NOSYNC") == 0;
  /* If using a temporary file, copy it to the output. */
  if (tmpfd > 0) {
    if (lseek(tmpfd, SEEK_SET, 0) != 0 || !obuf_copyfromfd(tmpfd, &msgbuf)) {
      close(tmpfd);
      tmpfd = 0;
      return &resp_writeerr;
    }
    close(tmpfd);
    tmpfd = 0;
  }
  /* Flush and close the output. */
  if ((dosync && !obuf_sync(&msgbuf))
      || !obuf_close(&msgbuf)) {
    reset();
    return &resp_writeerr;
  }
  if (link(tempname.s, destname.s) != 0) {
    reset();
    return &resp_writeerr;
  }
  if (dosync) {
    if ((fd = open(destpath.s, O_DIRECTORY | O_RDONLY)) < 0) {
      reset();
      return &resp_internal;
    }
    if (fsync(fd) != 0) {
      reset();
      return &resp_writeerr;
    }
    close(fd);
  }
  unlink(tempname.s);
  tempname.len = 0;
  destname.len = 0;
  return 0;
}

struct plugin backend = {
  .version = PLUGIN_VERSION,
  .reset = reset,
  .sender = sender,
  .recipient = recipient,
  .data_start = data_start,
  .data_block = data_block,
  .message_end = message_end,
};
