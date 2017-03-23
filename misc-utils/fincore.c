/*
 * fincore - count pages of file contents in core
 *
 * Copyright (C) 2017 Red Hat, Inc. All rights reserved.
 * Written by Masatake YAMATO <yamato@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>

#include "c.h"
#include "nls.h"
#include "closestream.h"
#include "xalloc.h"
#include "strutils.h"

#include "libsmartcols.h"

/* For large files, mmap is called in iterative way.
   Window is the unit of vma prepared in each mmap
   calling.

   Window size depends on page size.
   e.g. 128MB on x86_64. ( = N_PAGES_IN_WINDOW * 4096 ). */
#define N_PAGES_IN_WINDOW (32 * 1024)


struct colinfo {
	const char *name;
	double whint;
	int flags;
	const char *help;
};

enum {
	COL_PAGES,
	COL_SIZE,
	COL_FILE
};

static struct colinfo infos[] = {
	[COL_PAGES]  = { "PAGES",    1, SCOLS_FL_RIGHT, N_("number of memory page")},
	[COL_SIZE]   = { "SIZE",     5, SCOLS_FL_RIGHT, N_("size of the file")},
	[COL_FILE]   = { "FILE",     4, 0, N_("file name")},
};

static int columns[ARRAY_SIZE(infos) * 2] = {-1};
static size_t ncolumns;

struct fincore_control {
	const int pagesize;

	struct libscols_table *tb;		/* output */

	unsigned int bytes : 1;
};

static void __attribute__((__noreturn__)) usage(FILE *out)
{
	const char *p = program_invocation_short_name;

	if (!*p)
		p = "fincore";

	fputs(USAGE_HEADER, out);
	fprintf(out, _(" %s [options] file...\n"), program_invocation_short_name);
	fputs(USAGE_OPTIONS, out);
	fputs(USAGE_SEPARATOR, out);
	fputs(USAGE_HELP, out);
	fputs(USAGE_VERSION, out);
	fprintf(out, USAGE_MAN_TAIL("fincore(1)"));

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static int column_name_to_id(const char *name, size_t namesz)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(infos); i++) {
		const char *cn = infos[i].name;

		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return i;
	}
	warnx(_("unknown column: %s"), name);
	return -1;
}

static int get_column_id(int num)
{
	assert(num >= 0);
	assert((size_t) num < ncolumns);
	assert(columns[num] < (int) ARRAY_SIZE(infos));
	return columns[num];
}

static const struct colinfo *get_column_info(int num)
{
	return &infos[ get_column_id(num) ];
}

static int add_output_data(struct fincore_control *ctl,
			   const char *name,
			   off_t file_size,
			   off_t count_incore)
{
	size_t i;
	char *tmp;
	struct libscols_line *ln;

	assert(ctl);
	assert(ctl->tb);

	ln = scols_table_new_line(ctl->tb, NULL);
	if (!ln)
		err(EXIT_FAILURE, _("failed to initialize output line"));

	for (i = 0; i < ncolumns; i++) {
		switch(get_column_id(i)) {
		case COL_FILE:
			scols_line_set_data(ln, i, name);
			break;
		case COL_PAGES:
			xasprintf(&tmp, "%jd",  (intmax_t) count_incore);
			scols_line_refer_data(ln, i, tmp);
			break;
		case COL_SIZE:
			if (ctl->bytes)
				xasprintf(&tmp, "%jd", (intmax_t) file_size);
			else
				tmp = size_to_human_string(SIZE_SUFFIX_1LETTER, file_size);
			scols_line_refer_data(ln, i, tmp);
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int do_mincore(struct fincore_control *ctl,
		      void *window, const size_t len,
		      const char *name,
		      off_t *count_incore)
{
	static unsigned char vec[N_PAGES_IN_WINDOW];
	int n = (len / ctl->pagesize) + ((len % ctl->pagesize)? 1: 0);

	if (mincore (window, len, vec) < 0) {
		warn(_("failed to do mincore: %s"), name);
		return -errno;
	}

	while (n > 0)
	{
		if (vec[--n] & 0x1)
		{
			vec[n] = 0;
			(*count_incore)++;
		}
	}

	return 0;
}

static int fincore_fd (struct fincore_control *ctl,
		       int fd,
		       const char *name,
		       off_t file_size,
		       off_t *count_incore)
{
	size_t window_size = N_PAGES_IN_WINDOW * ctl->pagesize;
	off_t  file_offset;
	void  *window = NULL;
	int rc = 0;
	int warned_once = 0;

	for (file_offset = 0; file_offset < file_size; file_offset += window_size) {
		size_t len;

		len = file_size - file_offset;
		if (len >= window_size)
			len = window_size;

		window = mmap(window, len, PROT_NONE, MAP_PRIVATE, fd, file_offset);
		if (window == MAP_FAILED) {
			if (!warned_once) {
				rc = -EINVAL;
				warn(_("failed to do mmap: %s"), name);
				warned_once = 1;
			}
			break;
		}

		rc = do_mincore(ctl, window, len, name, count_incore);
		if (rc)
			break;

		munmap (window, len);
	}

	return rc;
}

/*
 * Returns: <0 on error, 0 success, 1 ignore.
 */
static int fincore_name(struct fincore_control *ctl,
			const char *name,
			struct stat *sb,
			off_t *count_incore)
{
	int fd;
	int rc = 0;

	if ((fd = open (name, O_RDONLY)) < 0) {
		warn(_("failed to open: %s"), name);
		return 0;
	}

	if (fstat (fd, sb) < 0) {
		warn(_("failed to do fstat: %s"), name);
		return -errno;
	}

	if (S_ISDIR(sb->st_mode))
		rc = 1;			/* ignore */

	else if (sb->st_size)
		rc = fincore_fd(ctl, fd, name, sb->st_size, count_incore);

	close (fd);
	return rc;
}

int main(int argc, char ** argv)
{
	int c;
	size_t i;
	int rc = EXIT_SUCCESS;

	struct fincore_control ctl = {
			.pagesize = getpagesize()
	};

	static const struct option longopts[] = {
		{ "version",    no_argument, NULL, 'V' },
		{ "help",	no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((c = getopt_long (argc, argv, "Vh", longopts, NULL)) != -1) {
		switch (c) {
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

	if (optind == argc) {
		warnx(_("no file specified"));
		errtryhelp(EXIT_FAILURE);
	}

	if (!ncolumns) {
		columns[ncolumns++] = COL_PAGES;
		columns[ncolumns++] = COL_SIZE;
		columns[ncolumns++] = COL_FILE;
	}

	scols_init_debug(0);
	ctl.tb = scols_new_table();
	if (!ctl.tb)
		err(EXIT_FAILURE, _("failed to create output table"));

	for (i = 0; i < ncolumns; i++) {
		const struct colinfo *col = get_column_info(i);

		if (!scols_table_new_column(ctl.tb, col->name, col->whint, col->flags))
			err(EXIT_FAILURE, _("failed to initialize output column"));
	}

	for(; optind < argc; optind++) {
		char *name = argv[optind];
		struct stat sb;
		off_t count_incore = 0;

		switch (fincore_name(&ctl, name, &sb, &count_incore)) {
		case 0:
			add_output_data(&ctl, name, sb.st_size, count_incore);
			break;
		case 1:
			break; /* ignore */
		default:
			rc = EXIT_FAILURE;
			break;
		}
	}

	scols_print_table(ctl.tb);
	scols_unref_table(ctl.tb);

	return rc;
}
