#include "cache.h"
#include "backup-log.h"
#include "lockfile.h"
#include "strbuf.h"

void bkl_append(struct strbuf *output, const char *path,
		const struct object_id *from,
		const struct object_id *to)
{
	if (oideq(from, to))
		return;

	strbuf_addf(output, "%s %s %s\t%s\n", oid_to_hex(from),
		    oid_to_hex(to), git_committer_info(0), path);
}

static int bkl_write_unlocked(const char *path, struct strbuf *new_log)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0666);
	if (fd == -1)
		return error_errno(_("unable to open %s"), path);
	if (write_in_full(fd, new_log->buf, new_log->len) < 0) {
		close(fd);
		return error_errno(_("unable to update %s"), path);
	}
	close(fd);
	return 0;
}

int bkl_write(const char *path, struct strbuf *new_log)
{
	struct lock_file lk;
	int ret;

	ret = hold_lock_file_for_update(&lk, path, LOCK_REPORT_ON_ERROR);
	if (ret == -1)
		return -1;
	ret = bkl_write_unlocked(path, new_log);
	/*
	 * We do not write the the .lock file and append to the real one
	 * instead to reduce update cost. So we can't commit even in
	 * successful case.
	 */
	rollback_lock_file(&lk);
	return ret;
}

int bkl_parse_entry(struct strbuf *sb, struct bkl_entry *re)
{
	char *email_end, *message;
	const char *p = sb->buf;

	/* old SP new SP name <email> SP time TAB msg LF */
	if (!sb->len || sb->buf[sb->len - 1] != '\n' ||
	    parse_oid_hex(p, &re->old_oid, &p) || *p++ != ' ' ||
	    parse_oid_hex(p, &re->new_oid, &p) || *p++ != ' ' ||
	    !(email_end = strchr(p, '>')) ||
	    email_end[1] != ' ')
		return -1;	/* corrupt? */
	re->email = p;
	re->timestamp = parse_timestamp(email_end + 2, &message, 10);
	if (!re->timestamp ||
	    !message || message[0] != ' ' ||
	    (message[1] != '+' && message[1] != '-') ||
	    !isdigit(message[2]) || !isdigit(message[3]) ||
	    !isdigit(message[4]) || !isdigit(message[5]))
		return -1; /* corrupt? */
	email_end[1] = '\0';
	re->tz = strtol(message + 1, NULL, 10);
	if (message[6] != '\t')
		message += 6;
	else
		message += 7;
	sb->buf[sb->len - 1] = '\0'; /* no LF */
	re->path = message;
	return 0;
}

static char *find_beginning_of_line(char *bob, char *scan)
{
	while (bob < scan && *(--scan) != '\n')
		; /* keep scanning backwards */
	/*
	 * Return either beginning of the buffer, or LF at the end of
	 * the previous line.
	 */
	return scan;
}

int bkl_parse_file_reverse(const char *path,
			   int (*parse)(struct strbuf *line, void *data),
			   void *data)
{
	struct strbuf sb = STRBUF_INIT;
	FILE *logfp;
	long pos;
	int ret = 0, at_tail = 1;

	logfp = fopen(path, "r");
	if (!logfp) {
		if (errno == ENOENT || errno == ENOTDIR)
			return 0;
		return -1;
	}

	/* Jump to the end */
	if (fseek(logfp, 0, SEEK_END) < 0)
		ret = error_errno(_("cannot seek back in %s"), path);
	pos = ftell(logfp);
	while (!ret && 0 < pos) {
		int cnt;
		size_t nread;
		char buf[BUFSIZ];
		char *endp, *scanp;

		/* Fill next block from the end */
		cnt = (sizeof(buf) < pos) ? sizeof(buf) : pos;
		if (fseek(logfp, pos - cnt, SEEK_SET)) {
			ret = error_errno(_("cannot seek back in %s"), path);
			break;
		}
		nread = fread(buf, cnt, 1, logfp);
		if (nread != 1) {
			ret = error_errno(_("cannot read %d bytes from %s"),
					  cnt, path);
			break;
		}
		pos -= cnt;

		scanp = endp = buf + cnt;
		if (at_tail && scanp[-1] == '\n')
			/* Looking at the final LF at the end of the file */
			scanp--;
		at_tail = 0;

		while (buf < scanp) {
			/*
			 * terminating LF of the previous line, or the beginning
			 * of the buffer.
			 */
			char *bp;

			bp = find_beginning_of_line(buf, scanp);

			if (*bp == '\n') {
				/*
				 * The newline is the end of the previous line,
				 * so we know we have complete line starting
				 * at (bp + 1). Prefix it onto any prior data
				 * we collected for the line and process it.
				 */
				strbuf_splice(&sb, 0, 0, bp + 1, endp - (bp + 1));
				scanp = bp;
				endp = bp + 1;
				ret = parse(&sb, data);
				strbuf_reset(&sb);
				if (ret)
					break;
			} else if (!pos) {
				/*
				 * We are at the start of the buffer, and the
				 * start of the file; there is no previous
				 * line, and we have everything for this one.
				 * Process it, and we can end the loop.
				 */
				strbuf_splice(&sb, 0, 0, buf, endp - buf);
				ret = parse(&sb, data);
				strbuf_reset(&sb);
				break;
			}

			if (bp == buf) {
				/*
				 * We are at the start of the buffer, and there
				 * is more file to read backwards. Which means
				 * we are in the middle of a line. Note that we
				 * may get here even if *bp was a newline; that
				 * just means we are at the exact end of the
				 * previous line, rather than some spot in the
				 * middle.
				 *
				 * Save away what we have to be combined with
				 * the data from the next read.
				 */
				strbuf_splice(&sb, 0, 0, buf, endp - buf);
				break;
			}
		}

	}
	if (!ret && sb.len)
		BUG("reverse reflog parser had leftover data");

	fclose(logfp);
	strbuf_release(&sb);
	return ret;
}

int bkl_parse_file(const char *path,
		   int (*parse)(struct strbuf *line, void *data),
		   void *data)
{
	struct strbuf sb = STRBUF_INIT;
	FILE *logfp;
	int ret = 0;

	logfp = fopen(path, "r");
	if (!logfp) {
		if (errno == ENOENT || errno == ENOTDIR)
			return 0;
		return -1;
	}

	while (!ret && !strbuf_getwholeline(&sb, logfp, '\n'))
		ret = parse(&sb, data);
	fclose(logfp);
	strbuf_release(&sb);
	return ret;
}
