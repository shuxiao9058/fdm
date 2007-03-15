/* $Id$ */

/*
 * Copyright (c) 2006 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <ctype.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fdm.h"

void	mail_free(struct mail *);

void
mail_open(struct mail *m, size_t size)
{
	memset(m, 0, sizeof m);

	m->size = size;
	m->space = m->size;
	m->body = -1;

	m->base = shm_malloc(&m->shm, m->size);
	cleanup_register(m->shm.name);

	m->off = 0;
	m->data = m->base + m->off;

	strb_create(&m->tags);
	ARRAY_INIT(&m->wrapped);
	m->attach = NULL;
	m->attach_built = 0;
}

void
mail_send(struct mail *m, struct msg *msg)
{
	struct mail	*mm = &msg->data.mail;

	memcpy(mm, m, sizeof *mm);
	ARRAY_INIT(&mm->wrapped);
	mm->attach = NULL;
}

void
mail_receive(struct mail *m, struct msg *msg)
{
	struct mail	*mm = &msg->data.mail;

	mm->tags = m->tags;
	m->tags = NULL;
	mm->attach = m->attach;
	m->attach = NULL;

	mm->auxfree = m->auxfree;
	m->auxfree = NULL;
	mm->auxdata = m->auxdata;
	m->auxdata = NULL;

	mail_destroy(m);

	memcpy(m, mm, sizeof *m);

	m->base = shm_reopen(&m->shm);
	cleanup_register(m->shm.name);

	m->data = m->base + m->off;
	ARRAY_INIT(&m->wrapped);
}

void
mail_free(struct mail *m)
{
	if (m->attach != NULL)
		attach_free(m->attach);
	if (m->tags != NULL)
		strb_destroy(&m->tags);
	ARRAY_FREE(&m->wrapped);

	if (m->auxfree != NULL && m->auxdata != NULL)
		m->auxfree(m->auxdata);
}

void
mail_close(struct mail *m)
{
	char	path[MAXPATHLEN];

	mail_free(m);
	if (m->base != NULL) {
		strlcpy(path, m->shm.name, sizeof path);
		shm_free(&m->shm);
		cleanup_deregister(path);
	}
}

void
mail_destroy(struct mail *m)
{
	char	path[MAXPATHLEN];

	mail_free(m);
	if (m->base != NULL) {
		strlcpy(path, m->shm.name, sizeof path);
		shm_destroy(&m->shm);
		cleanup_deregister(path);
	}
}

void
resize_mail(struct mail *m, size_t size)
{
	if (SIZE_MAX - m->off < size)
		fatalx("resize_mail: SIZE_MAX - m->off < size");
	while (m->space <= (m->off + size)) {
		m->base = shm_realloc(&m->shm, 2, m->space);
		m->space *= 2;
	}
	m->data = m->base + m->off;
}

char *
rfc822_time(time_t t, char *buf, size_t len)
{
	struct tm	*tm;
	size_t		 n;

	tm = localtime(&t);
	if ((n = strftime(buf, len, "%a, %d %b %Y %H:%M:%S %z", tm)) == 0)
		return (NULL);
	if (n == len)
		return (NULL);
	return (buf);
}

int
printpath(char *buf, size_t len, const char *fmt, ...)
{
	va_list	ap;
	int	n;

	if (len > INT_MAX) {
		errno = ENAMETOOLONG;
		return (1);
	}

	va_start(ap, fmt);
	n = xvsnprintf(buf, len, fmt, ap);
	va_end(ap);

	if ((size_t) n > len) {
		errno = ENAMETOOLONG;
		return (1);
	}

	return (0);
}
int
openlock(const char *path, u_int locks, int flags, mode_t mode)
{
	char		*lock;
	int	 	 fd, error;
	struct flock	 fl;

	if (locks & LOCK_DOTLOCK) {
		xasprintf(&lock, "%s.lock", path);
 		fd = open(lock, O_WRONLY|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR);
		if (fd == -1) {
			if (errno == EEXIST)
				errno = EAGAIN;
			xfree(lock);
			return (-1);
		}
		close(fd);
		cleanup_register(lock);
	}

	fd = open(path, flags, mode);

	if (fd != -1 && locks & LOCK_FLOCK) {
		if (flock(fd, LOCK_EX|LOCK_NB) != 0) {
			if (errno == EWOULDBLOCK)
				errno = EAGAIN;
			goto error;
		}
	}

	if (fd != -1 && locks & LOCK_FCNTL) {
		memset(&fl, 0, sizeof fl);
		fl.l_start = 0;
		fl.l_len = 0;
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		if (fcntl(fd, F_SETLK, &fl) == -1) {
			/* fcntl already returns EAGAIN if needed */
			goto error;
		}
	}

	if (locks & LOCK_DOTLOCK)
		xfree(lock);
	return (fd);

error:
	error = errno;
	close(fd);
	if (locks & LOCK_DOTLOCK) {
		unlink(lock);
		cleanup_deregister(lock);
		xfree(lock);
	}
	errno = error;
	return (-1);
}

void
closelock(int fd, const char *path, u_int locks)
{
	char	*lock;

	if (locks & LOCK_DOTLOCK) {
		xasprintf(&lock, "%s.lock", path);
		unlink(lock);
		cleanup_deregister(lock);
		xfree(lock);
	}

	close(fd);
}

int
checkperms(const char *hdr, const char *path, int *exists)
{
	struct stat	sb;
	gid_t		gid;
	mode_t		mode;

	if (stat(path, &sb) != 0) {
		if (errno == ENOENT) {
			*exists = 0;
			return (0);
		}
		return (1);
	}
	*exists = 1;

	mode = (S_ISDIR(sb.st_mode) ? DIRMODE : FILEMODE) & ~conf.file_umask;
	if ((sb.st_mode & DIRMODE) != mode) {
		log_warnx("%s: %s: bad permissions: %o%o%o, should be %o%o%o",
		    hdr, path, MODE(sb.st_mode), MODE(mode));
	}

	if (sb.st_uid != getuid()) {
		log_warnx("%s: %s: bad owner: %lu, should be %lu", hdr, path,
		    (u_long) sb.st_uid, (u_long) getuid());
	}

	gid = conf.file_group;
	if (gid == NOGRP)
		gid = getgid();
	if (sb.st_gid != gid) {
		log_warnx("%s: %s: bad group: %lu, should be %lu", hdr, path,
		    (u_long) sb.st_gid, (u_long) gid);
	}

	return (0);
}

void
line_init(struct mail *m, char **line, size_t *len)
{
	char	*ptr;

	*line = m->data;

	ptr = memchr(m->data, '\n', m->size);
	if (ptr == NULL)
		*len = m->size;
	else
		*len = (ptr - *line) + 1;
}

void
line_next(struct mail *m, char **line, size_t *len)
{
	char	*ptr;

	*line += *len;
	if (*line == m->data + m->size) {
		*line = NULL;
		return;
	}

	ptr = memchr(*line, '\n', (m->data + m->size) - *line);
	if (ptr == NULL)
		*len = (m->data + m->size) - *line;
	else
		*len = (ptr - *line) + 1;
}

int
remove_header(struct mail *m, const char *hdr)
{
	char	*ptr;
	size_t	 len;

	if ((ptr = find_header(m, hdr, &len, 0)) == NULL)
		return (1);

	/* include the \n */
	len++;

	/* remove the header */
	memmove(ptr, ptr + len, m->size - len - (ptr - m->data));
	m->size -= len;
 	if (m->body != -1)
		m->body -= len;

	return (0);
}

int
insert_header(struct mail *m, const char *before, const char *fmt, ...)
{
	va_list		 ap;
	char		*hdr, *ptr;
	size_t		 hdrlen, len, off;

	if (before != NULL) {
		/* insert before header */
		ptr = find_header(m, before, &len, 0);
		if (ptr == NULL)
			return (1);
		off = ptr - m->data;
	} else {
		/* insert at the end */
		if (m->body == -1)
			off = m->size;
		else if (m->body < 1)
			off = 0;
		else
			off = m->body - 1;
	}

	va_start(ap, fmt);
	hdrlen = xvasprintf(&hdr, fmt, ap);
	va_end(ap);

	/* include the \n */
	hdrlen++;

	/* make space for the header */
	resize_mail(m, m->size + hdrlen);
	ptr = m->data + off;
	memmove(ptr + hdrlen, ptr, m->size - off);

	/* copy the header */
	memcpy(ptr, hdr, hdrlen - 1);
	ptr[hdrlen - 1] = '\n';
	m->size += hdrlen;
 	if (m->body != -1)
		m->body += hdrlen;

	xfree(hdr);
	return (0);
}

char *
find_header(struct mail *m, const char *hdr, size_t *len, int value)
{
	char	*ptr, *end, *out;
	size_t	 hdrlen;

	hdrlen = strlen(hdr) + 1; /* include : */

	end = m->data + (m->body == -1 ? m->size : (size_t) m->body);
	ptr = m->data;
	if (hdrlen > (size_t) (end - ptr))
		return (NULL);
	while (ptr[hdrlen - 1] != ':' ||
	    strncasecmp(ptr, hdr, hdrlen - 1) != 0) {
		ptr = memchr(ptr, '\n', end - ptr);
		if (ptr == NULL)
			return (NULL);
		ptr++;
		if (hdrlen > (size_t) (end - ptr))
			return (NULL);
	}

	out = ptr + hdrlen;
	ptr = memchr(out, '\n', end - out);
	if (ptr == NULL)
		*len = end - out;
	else
		*len = ptr - out;

	/* header must be followed by space */
	if (!isspace((int) *out))
		return (NULL);

	/* sort out what is actually returned */
	if (value) {
		/* strip any following space */
		while (isspace((int) *out)) {
			out++;
			(*len)--;
		}
	} else {
		/* move back to the start of the header */
		out -= hdrlen;
		(*len) += hdrlen;
	}

	if (len == 0)
		return (NULL);

	return (out);
}

struct strings *
find_users(struct mail *m)
{
	struct passwd	*pw;
	struct strings	*users;
	u_int	 	 i, j;
	char		*hdr, *ptr, *dptr, *dom;
	size_t	 	 len, alen;

	users = xmalloc(sizeof *users);
	ARRAY_INIT(users);

	for (i = 0; i < ARRAY_LENGTH(conf.headers); i++) {
		if (*ARRAY_ITEM(conf.headers, i, char *) == '\0')
			continue;

		hdr = find_header(m, ARRAY_ITEM(conf.headers, i, char *), 
		    &len, 1);
		if (hdr == NULL || len == 0)
			continue;
		while (isspace((int) *hdr)) {
			hdr++;
			len--;
		}
		if (*hdr == '\0')
			continue;

		while (len > 0) {
			ptr = find_address(hdr, len, &alen);
			if (ptr == NULL)
				break;

			dptr = ((char *) memchr(ptr, '@', alen)) + 1;
			for (j = 0; j < ARRAY_LENGTH(conf.domains); j++) {
				dom = ARRAY_ITEM(conf.domains, j, char *);
				if (fnmatch(dom, dptr, FNM_CASEFOLD) != 0)
					continue;
				*--dptr = '\0';
				pw = getpwnam(ptr);
				if (pw != NULL)
					ARRAY_ADD(users, pw->pw_uid, uid_t);
				endpwent();
				*dptr++ = '@';
				break;
			}

			len -= (ptr - hdr) + alen;
			hdr = ptr + alen;
		}
	}

	if (ARRAY_EMPTY(users)) {
		ARRAY_FREE(users);
		xfree(users);
		return (NULL);
	}
	return (weed_strings(users));
}

char *
find_address(char *hdr, size_t len, size_t *alen)
{
	char	*ptr;
	size_t	 off, pos;

	for (off = 0; off < len; off++) {
		switch (hdr[off]) {
		case '"':
			off++;
			while (off < len && hdr[off] != '"')
				off++;
			if (off < len)
				off++;
			break;
		case '<':
			off++;
			ptr = memchr(hdr + off, '>', len - off);
			if (ptr == NULL)
				break;
			*alen = ptr - (hdr + off);
			for (pos = 0; pos < *alen; pos++) {
				if (!isaddr(hdr[off + pos]))
					break;
			}
			if (pos != *alen)
				break;
			ptr = hdr + off;
			if (*alen == 0 || memchr(ptr, '@', *alen) == NULL)
				break;
			if (ptr[0] == '@' || ptr[*alen - 1] == '@')
				break;
			return (ptr);
		}
	}

	/* no address found. try the whole header */
	*alen = 0;
	for (*alen = 0; *alen < len; (*alen)++) {
		if (!isaddr(hdr[*alen]))
			break;
	}
	if (*alen == 0 || memchr(hdr + off, '@', *alen) == NULL)
		return (NULL);
	if (hdr[off] == '@' || hdr[*alen - 1] == '@')
		return (NULL);
	return (hdr);
}

void
trim_from(struct mail *m)
{
	char	*ptr;
	size_t	 len;

	if (m->data == NULL || m->size < 5 || strncmp(m->data, "From ", 5) != 0)
		return;

	ptr = memchr(m->data, '\n', m->size);
	if (ptr == NULL)
		ptr = m->data + m->size;
	else
		ptr++;
	len = ptr - m->data;

	m->size -= len;
	m->off += len;
	m->data = m->base + m->off;
	if (m->body != -1)
		m->body -= len;
}

char *
make_from(struct mail *m)
{
	time_t	 t;
	char	*s, *from = NULL;
	size_t	 fromlen = 0;

	from = find_header(m, "from", &fromlen, 1);
	if (from != NULL && fromlen > 0)
		from = find_address(from, fromlen, &fromlen);
 	if (fromlen > INT_MAX)
		from = NULL;
	if (from == NULL) {
		from = conf.info.user;
		fromlen = strlen(from);
	}

	t = time(NULL);
	xasprintf(&s, "From %.*s %.24s", (int) fromlen, from, ctime(&t));
	return (s);
}

/*
 * Sometimes mail has wrapped header lines, this undoubtedly looks neat but
 * makes them a pain to match using regexps. We build a list of the newlines
 * in all the wrapped headers in m->wrapped, and can then quickly unwrap them
 * for regexp matching and wrap them again for delivery.
 */
u_int
fill_wrapped(struct mail *m)
{
	char		*ptr;
	size_t	 	 end, off;
	u_int		 n;

	if (!ARRAY_EMPTY(&m->wrapped))
		fatalx("fill_wrapped: mail already wrapped");

	ARRAY_INIT(&m->wrapped);

	end = m->body == -1 ? m->size : (size_t) m->body;
	ptr = m->data;

	n = 0;
	for (;;) {
		ptr = memchr(ptr, '\n', m->size - (ptr - m->data));
		if (ptr == NULL)
			break;
		ptr++;
		off = ptr - m->data;
		if (off >= end)
			break;

		/* check if the line starts with whitespace */
		if (!isblank((int) *ptr))
			continue;

		/* save the position */
		ARRAY_ADD(&m->wrapped, off - 1, size_t);
		n++;
	}

	return (n);
}

void
set_wrapped(struct mail *m, char ch)
{
	u_int	i;

	for (i = 0; i < ARRAY_LENGTH(&m->wrapped); i++)
		m->data[ARRAY_ITEM(&m->wrapped, i, size_t)] = ch;
}
