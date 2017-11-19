/*
 * Copyright 2016-2017 Diomidis Spinellis
 *
 * Wrap any command to participate in the dgsh negotiation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/*
 * Examples:
 * dgsh-wrap -i 0 yes | fsck
 * tar cf - / | dgsh-wrap -o 0 dd of=/dev/st0
 * ls | dgsh-wrap /usr/bin/sort -k5n | more
 */

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>

#include "dgsh.h"
#include "dgsh-debug.h"		/* DPRINTF(4, ) */

/* Determine if the OS splits shebang argument or not */
#if __APPLE__
#include "TargetConditionals.h"
#if TARGET_OS_MAC
#define OS_SPLITS_SHEBANG_ARGS 1
#endif
#endif

static void
usage(void)
{
	fputs("Usage:\tdgsh-wrap [-S] [-i 0|a] [-o 0|a] [-eIO] program [program-arguments ...]\n"
		"\tdgsh-wrap -s [-i 0|a] [-o 0|a] [-eIO] [program-arguments ...]\n"
		"-e\t"		"Process <| and >| embedded in arguments\n"
		"-i 0|a\t"	"Process no (0) or arbitrary (a) input channels\n"
		"-I\t"		"Do not provide standard input as a <| arg\n"
		"-o 0|a\t"	"Process no (0) or arbitrary (a) output channels\n"
		"-O\t"		"Do not provide standard output as a >| arg\n"
		"-S\t"		"Process flags and program as a #! interpreter\n"
		"-s\t"		"Process flags as a #! interpreter\n"
		"\t"		"(-S or -s must be the first flag of shebang line)\n"
		"-x\t"		"Wrap a non-dgsh command that will exec a dgsh one\n",
		stderr);
	exit(1);
}

static void *
xmalloc(size_t size)
{
	void *r = malloc(size);
	if (r == NULL)
		err(1, "malloc out of memory");
	return r;
}

char *
xstrdup(const char *s)
{
	void *r = strdup(s);
	if (r == NULL)
		err(1, "stdup out of memory");
	return r;
}

static void *
xrealloc(void *ptr, size_t size)
{
	void *r = realloc(ptr, size);
	if (r == NULL)
		err(1, "realloc out of memory");
	return r;
}


/*
 * Remove from the PATH environment variable an entry with the specified string
 */
static void
remove_from_path(const char *string)
{
	char *start, *end, *path, *strptr;

	path = getenv("PATH");
	if (!path)
		return;
	path = xstrdup(path);
	if (!path)
		err(1, "Error allocating path copy");
	strptr = strstr(path, string);
	if (!strptr)
		return;
	/* Find start of this path element */
	for (start = strptr; start != path && *start != ':'; start--)
		;
	/* Find end of this path element */
	for (end = strptr; *end && *end != ':'; end++)
		;
	/*
	 * At this point:
	 * start can point to : or path,
	 * end can point to : or \0.
	 * Work through all cases.
	 */
	if (*end == '\0')
		*start = '\0';
	else if (*start == ':')
		memmove(start, end, strlen(end));
	else /* first element, followed by another */
		memmove(start, end + 1, strlen(end + 1));

	if (setenv("PATH", path, 1) != 0)
		err(1, "Setting path");

	free(path);
}

static void
dump_args(int argc, char *argv[])
{
	int i;

	for (i = 0; i <= argc; i++)
		DPRINTF(4, "argv[%d]: [%s]", i, argv[i]);
}
/*
 * On operating systems that pass unsplit all the #! line arguments
 * to the interpreter, process the arguments of a #! invocation to make
 * them equivalent to a command-line one.
 * This entails tokenizing argv[1], which contains all the #! line
 * after the name of the interpreter.
 *
 * Example:
 * Input arguments:
 * argv[0]: /usr/local/libexec/dgsh/dgsh-wrap
 * argv[1]: -S -d /bin/uname
 * argv[2]: /usr/local/libexec/dgsh/uname
 * argv[3]: -s
 * argv[4]: NULL
 * Output arguments:
 * argv[0]: /usr/local/libexec/dgsh/dgsh-wrap
 * argv[1]: -S
 * argv[2]: -d
 * argv[3]: /bin/uname
 * argv[4]: /usr/local/libexec/dgsh/uname
 * argv[5]: -s
 * argv[6]: NULL
 */
static void
split_argv(int *argcp, char ***argvp)
{
	int argc = *argcp;
	char **argv = *argvp;

	/* Tokenize argv[1] into multiple arguments */
	argv = xmalloc((argc + 1) * sizeof(char *));
	memcpy(argv, *argvp, (argc + 1) * sizeof(char *));
	int i = 1;
	const char *delim = " \t";
	char *p = strtok(xstrdup(argv[1]), delim);
	assert(p != NULL);	/* One arg (-s or -S) is guaranteed to exist */
	for (;;) {
		argv[i] = p;
		p = strtok(NULL, delim);
		if (p == NULL)
			break;
		/* Make room for new string */
		argc += 1;
		argv = xrealloc(argv, (argc + 1) * sizeof(char *));
		memmove(argv + i + 2, argv + i + 1,
				(argc - i - 1) * sizeof(char *));
		i++;
	}
	*argvp = argv;
	*argcp = argc;
	DPRINTF(4, "Arguments after split_argv");
	dump_args(*argcp, *argvp);
}

/*
 * -S: remove OS-supplied script path name
 * Input arguments:
 * argv[0]: /usr/local/libexec/dgsh/dgsh-wrap
 * argv[1]: -S -d /bin/uname
 * argv[2]: /usr/local/libexec/dgsh/uname
 * argv[3]: -s
 * Arguments at this point:
 * argv[0]: /usr/local/libexec/dgsh/dgsh-wrap
 * argv[1]: -S
 * argv[2]: -d
 * argv[3]: /bin/uname			<- argv[optind]
 * argv[4]: /usr/local/libexec/dgsh/uname
 * argv[5]: -s
 * Result arguments:
 * argv[0]: /usr/local/libexec/dgsh/dgsh-wrap
 * argv[1]: -S
 * argv[2]: -d
 * argv[3]: /bin/uname
 * argv[4]: -s
 */
static void
remove_os_script_path(char **argv, int *argc, int optind)
{
	memmove(argv + optind + 1, argv + optind + 2, (*argc - optind) * sizeof(char *));
	*argc -= 1;
	argv[*argc] = NULL;
}

/* Remove absolute path from specified string
 * Example:
 * -s: Remove absolute path from argv[optind]
 * Input arguments:
 * argv[0]: /usr/local/libexec/dgsh/dgsh-wrap
 * argv[1]: -s -d
 * argv[2]: /usr/local/libexec/dgsh/uname
 * argv[3]: -s
 * Arguments at this point:
 * argv[0]: /usr/local/libexec/dgsh/dgsh-wrap
 * argv[1]: -s
 * argv[2]: -d
 * argv[3]: /usr/local/libexec/dgsh/uname	<- argv[optind]
 * argv[4]: -s
 * Result arguments:
 * argv[0]: /usr/local/libexec/dgsh/dgsh-wrap
 * argv[1]: -s
 * argv[2]: -d
 * argv[3]: uname
 * argv[4]: -s
 */
static void
remove_absolute_path(char *s)
{
	char *p = strrchr(s, '/');
	if (p)
		memmove(s, p + 1, strlen(p) + 1);
}

/*
 * Replace an instance of the string special (e.g. "<|") embedded in arg
 * with /dev/fd/N, where N is the integer pointed
 * by fdptr, and increase fdptr to point to the next integer.
 * Return true if a replacement was made, false if not.
 */
static bool
process_embedded_io_arg(char **arg, const char *special, int **fdptr)
{
	char *p = strstr(*arg, special);
	if (p == NULL)
		return false;
	*p = 0;
	char *before = *arg;
	char *after = p + strlen(special);
	if (asprintf(arg, "%s/dev/fd/%d%s", before, **fdptr, after) == -1)
		err(1, "asprintf out of memory");
	(*fdptr)++;
	return true;
}

/*
 * Replace an instance of the string special (e.g. "<|") matching an arg
 * with /dev/fd/N, where N is the integer pointed
 * by fdptr, and increase fdptr to point to the next integer.
 * If special is null, then the replacement is always made.
 */
static void
process_standalone_io_arg(char **arg, const char *special, int **fdptr)
{
	if (special != NULL && strcmp(*arg, special) != 0)
		return;
	if (asprintf(arg, "/dev/fd/%d", **fdptr) == -1)
		err(1, "asprintf out of memory");
	(*fdptr)++;
}

/*
 * Increment the channels specified by the given variable.
 * Ensure that the corresponding variable is not
 * already set to an arbitrary number of channels.
 */
static void
increment_channels(int *var)
{
	if (*var == -1) {
		fputs("I/O channel arguments cannot be combined with an arbitrary I/O file specification\n",
				stderr);
		exit(1);
	}
	(*var)++;
}

int
main(int argc, char *argv[])
{
	int nflags = 0;
	bool negotiation_flags = false;
	int ninputs = 1, noutputs = 1;
	bool xflag = false;
	int ch, i;
	char *p;
	char *debug_level;
	char *guest_program_name;
	/* Option-dependent flags */
	bool program_from_os = false, program_supplied = false;
	bool embedded_args = false;
	/* Pass stdin/stdout as a command-line argument */
	bool stdin_as_arg = true, stdout_as_arg = true;
	bool supply_input_args = false, supply_output_args = false;


	debug_level = getenv("DGSH_DEBUG_LEVEL");
	if (debug_level)
		dgsh_debug_level = atoi(debug_level);

	/* Preclude recursive wrapping */
	DPRINTF(4, "PATH before: [%s]", getenv("PATH"));
	remove_from_path("libexec/dgsh");
	DPRINTF(4, "PATH after: [%s]", getenv("PATH"));

	DPRINTF(4, "Initial arguments");
	dump_args(argc, argv);

	/* Check for #! (shebang) interpreter argument processing */
#if !defined(OS_SPLITS_SHEBANG_ARGS)
	if (argc >= 2 && argv[1][0] == '-' && tolower(argv[1][1]) == 's')
		split_argv(&argc, &argv);
#endif

	/*
	 * The + argument to getopt causes it on glibc to stop processing on
	 * first non-flag argument.
	 * Therefore, adjust argc, argv on entry and optind on exit.
	 */
        while ((ch = getopt(argc, argv, "+ei:Io:OSsx")) != -1) {
		DPRINTF(4, "getopt switch=%c", ch);
		switch (ch) {
		case 'i':
			nflags++;
			negotiation_flags = true;
			if (strcmp(optarg, "0") == 0)
				ninputs = 0;
			else if (strcmp(optarg, "a") == 0) {
				ninputs = -1;
				supply_input_args = true;
			} else
				usage();
			break;
		case 'e':
			embedded_args = true;
			negotiation_flags = true;
			nflags++;
			break;
		case 'I':
			stdin_as_arg = false;
			negotiation_flags = true;
			nflags++;
			break;
		case 'o':
			nflags++;
			negotiation_flags = true;
			if (strcmp(optarg, "0") == 0)
				noutputs = 0;
			else if (strcmp(optarg, "a") == 0) {
				noutputs = -1;
				supply_output_args = true;
			} else
				usage();
			break;
		case 'O':
			stdout_as_arg = false;
			negotiation_flags = true;
			nflags++;
			break;
		case 'S':
			/* Complain this is not the first flag */
			if (nflags) {
				fputs("-S must be the first provided flag\n",
						stderr);
				usage();
			}
			nflags++;
			program_supplied = true;
			break;
		case 's':
			/* Complain this is not the first flag */
			if (nflags) {
				fputs("-s must be the first provided flag\n",
					stderr);
				usage();
			}
			nflags++;
			program_from_os = true;
			break;
		case 'x':
			xflag = true;
			break;
		case '?':
		default:
			usage();
		}
	}
	DPRINTF(3, "After getopt: ninputs=%d, noutputs=%d optind=%d argv[optind]=%s",
			ninputs, noutputs, optind, argv[optind]);
	DPRINTF(3, "program_supplied=%d", program_supplied);

	if (optind >= argc)
		usage();

	if (xflag && negotiation_flags) {
		fputs("-x cannot be combined with I/O specifications\n",
			stderr);
		usage();
	}

	/*
	 * Process argv[2], which is the name of the script
	 * supplied by the kernel to the interpreter, i.e.
	 * the name of the program we are being executed as.
	 */
	if (program_supplied && argc > optind)
		remove_os_script_path(argv, &argc, optind);
	else if (program_from_os)
		remove_absolute_path(argv[optind]);

	DPRINTF(4, "Arguments after processing program name (optind=%d)", optind);
	dump_args(argc, argv);

	if (xflag) {
		/*
		 * Execute (non-dgsh) command, which will execute a dgsh
		 * command, which will negotiate on our behalf
		 */
		execvp(argv[optind], argv + optind);

		err(1, "Unable to execute %s", argv[optind]);
		return 1;
	}

	/* Obtain guest program name (without path) */
	guest_program_name = xstrdup(argv[optind]);
	remove_absolute_path(guest_program_name);
	DPRINTF(4, "guest_program_name: %s", guest_program_name);

	/*
	 * Adjust ninputs and noutputs by special arguments
	 * "<|" and ">|", which mean input from or output to
	 * /dev/fd/N
	 */
	DPRINTF(4, "embedded_args=%d", embedded_args);
	for (i = optind + 1; i < argc; i++) {
		if (embedded_args) {
			for (p = argv[i]; p = strstr(p, "<|"); p += 2)
				increment_channels(&ninputs);
			for (p = argv[i]; p = strstr(p, ">|"); p += 2)
				increment_channels(&noutputs);
		} else {
			if (strcmp(argv[i], "<|") == 0)
				increment_channels(&ninputs);
			if (strcmp(argv[i], ">|") == 0)
				increment_channels(&noutputs);
		}
	}

	/*
	 * Adjust for the default implicit I/O channel.
	 * E.g. if two <| are specified, ninputs will be 3 at this point,
	 * whereas we want it to be 2.
	 */
	if (stdin_as_arg && ninputs > 1)
		ninputs--;
	if (stdout_as_arg && noutputs > 1)
		noutputs--;

	/* Participate in negotiation */
	DPRINTF(3, "calling negotiate with ninputs=%d noutputs=%d", ninputs, noutputs);
	int *input_fds = NULL, *output_fds = NULL;
	dgsh_negotiate(DGSH_HANDLE_ERROR, guest_program_name,
					&ninputs, &noutputs,
					&input_fds, &output_fds);

	/*
	 * Substitute special arguments "<|" and ">|" with or add file descriptor
	 * paths /dev/fd/N using the fds received from negotiation.
	 */
	int *inptr = stdin_as_arg ? input_fds : input_fds + 1;
	if (supply_input_args) {
		if (!stdin_as_arg)
			ninputs--;

		/* Create space for arguments to add */
		char **nargv = xmalloc((argc + ninputs + 1) * sizeof(char *));
		memcpy(nargv, argv, argc * sizeof(char *));
		memset(argv + argc, 0, (ninputs + 1) * sizeof(char *));
		argv = nargv;

		/* Add arguments */
		for (i = argc; i < argc + ninputs; i++)
			process_standalone_io_arg(&argv[i], NULL, &inptr);
		argc += ninputs;
		argv[argc] = NULL;
	} else {
		for (i = optind + 1; i < argc; i++) {
			if (embedded_args)
				while (process_embedded_io_arg(&argv[i], "<|", &inptr))
					;
			else
				process_standalone_io_arg(&argv[i], "<|", &inptr);
		}
	}
	int *outptr = stdout_as_arg ? output_fds : output_fds + 1;
	if (supply_output_args) {
		if (!stdout_as_arg)
			noutputs--;

		/* Create space for arguments to add */
		char **nargv = xmalloc((argc + noutputs + 1) * sizeof(char *));
		memcpy(nargv, argv, argc * sizeof(char *));
		memset(argv + argc, 0, (noutputs + 1) * sizeof(char *));
		argv = nargv;

		/* Add arguments */
		for (i = argc; i < argc + noutputs; i++)
			process_standalone_io_arg(&argv[i], NULL, &outptr);
		argc += noutputs;
		argv[argc] = NULL;
	} else {
		for (i = optind + 1; i < argc; i++) {
			if (embedded_args)
				while (process_embedded_io_arg(&argv[i], ">|", &outptr))
					;
			else
				process_standalone_io_arg(&argv[i], ">|", &outptr);
		}
	}
	DPRINTF(4, "Arguments to execvp after substitung <| and >|");
	dump_args(argc - optind, argv + optind);

	/* Execute command */
	execvp(argv[optind], argv + optind);

	err(1, "Unable to execute %s", argv[optind]);
	return 1;
}
