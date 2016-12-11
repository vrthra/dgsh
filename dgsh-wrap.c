/*
 * Copyright 2016 Diomidis Spinellis
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
 * dgsh-wrap yes | fsck
 * tar cf - / | dgsh-wrap dd of=/dev/st0
 * ls | dgsh-wrap sort -k5n | more
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <err.h>

#include "dgsh.h"
#include "dgsh-negotiate.h"

static const char *program_name;
static char *guest_program_name = NULL;

static void
usage(void)
{
	fprintf(stderr, "Usage: %s [-d | -m] program [arguments ...]\n"
			"-d"		"Requires no input; d for deaf\n"
			"-m"		"Provides no output; m for mute\n",
		program_name);
	exit(1);
}

int
main(int argc, char *argv[])
{
	int pos = 1;
	int *ninputs = NULL, *noutputs = NULL;
	int *input_fds = NULL;

	DPRINTF("argc: %d\n", argc);
	int k = 0;
	for (k = 0; k < argc; k++)
		DPRINTF("argv[%d]: %s\n", k, argv[k]);

	program_name = argv[0];

	/* Parse any arguments to dgsh-wrap
	 * Tried getopt, but it swallows spaces in this case
	 */
	if (argv[1][0] == '-') {
		if (argv[1][1] == 'd') {
			ninputs = (int *)malloc(sizeof(int));
			*ninputs = 0;
			pos++;
			// argv[1] may carry also the guest program's name
			if (argv[1][2] == ' ')
				guest_program_name = &argv[1][3];
		} else if (argv[1][1] == 'm') {
			noutputs = (int *)malloc(sizeof(int));
			*noutputs = 0;
			pos++;
			if (argv[1][2] == ' ')
				guest_program_name = &argv[1][3];
		} else
			usage();
	}

	if (!guest_program_name) {
		guest_program_name = argv[pos];
		pos++;
	}
	DPRINTF("guest_program_name: %s\n", guest_program_name);

	int exec_argv_len = argc - 1;
	char *exec_argv[exec_argv_len];
	int i, j;

	exec_argv[0] = guest_program_name;

	/* Arguments might contain the dgsh-wrap script to executable
	 * Skip the argv item that contains the wrapper script
	 */
	int cmp = 0, compare_chars = strlen(argv[0]) - strlen("dgsh-wrap");
	DPRINTF("argv[0]: %s, argv[2]: %s, compare_chars: %d\n",
			argv[0], argv[pos], compare_chars);
	if (compare_chars > 0 &&
			!(cmp = strncmp(argv[pos], argv[0], compare_chars)))
		pos++;
	DPRINTF("cmp: %d, pos: %d\n", cmp, pos);

	// Pass argv arguments to exec_argv for exec() call.
	for (i = pos, j = 1; i < argc; i++, j++)
		exec_argv[j] = argv[i];
	exec_argv[j] = NULL;

	// Mark special argument "<|" that means input from /proc/self/fd/x
	for (k = 0; exec_argv[k] != NULL; k++) {	// exec_argv[argc - 1] = NULL
		DPRINTF("exec_argv[%d]: %s\n", k, exec_argv[k]);
		char *m = NULL;
		if (!strcmp(exec_argv[k], "<|") ||
				(m = strstr(exec_argv[k], "<|"))) {
			if (!ninputs) {
				ninputs = (int *)malloc(sizeof(int));
				*ninputs = 1;
			}
			if (!m)
				(*ninputs)++;
			while (m) {
				(*ninputs)++;
				m += 2;
				m = strstr(m, "<|");
			}
			DPRINTF("ninputs: %d\n", *ninputs);
		}
	}

	/* Build command title to be used in negotiation
	 * Include the first two arguments
	 */
	DPRINTF("argc: %d\n", argc);
	char negotiation_title[100];
	if (argc >= 5)	// [4] does not exist, [3] is NULL
		snprintf(negotiation_title, 100, "%s %s %s",
				guest_program_name, exec_argv[1], exec_argv[2]);
	else if (argc == 4) // [3] does not exist, [2] is NULL
		snprintf(negotiation_title, 100, "%s %s",
				guest_program_name, exec_argv[1]);
	else
		snprintf(negotiation_title, 100, "%s", guest_program_name);

	// Participate in negotiation
	int status;
	if ((status = dgsh_negotiate(negotiation_title, ninputs, noutputs, &input_fds,
				NULL)) != 0)
		errx(1, "dgsh negotiation failed for %s with status code %d\n",
				negotiation_title, status);

	int n = 1;
	char *fds[argc - 2];		// /proc/self/fd/x or arg=/proc/self/fd/x
	memset(fds, 0, sizeof(fds));

	if (ninputs)
		DPRINTF("%s returned %d input fds\n",
				negotiation_title, *ninputs);
	/* Substitute special argument "<|" with /proc/self/fd/x received
	 * from negotiation
	 */
	for (k = 0; exec_argv[k] != NULL; k++) {	// exec_argv[argc - 1] = NULL
		char *m = NULL;
		DPRINTF("exec_argv[%d] to sub: %s\n", k, exec_argv[k]);
		if (!strcmp(exec_argv[k], "<|") ||
			(m = strstr(exec_argv[k], "<|"))) {

			size_t size = sizeof(char) *
				(strlen(exec_argv[k]) + 20 * *ninputs);
			DPRINTF("fds[k] size: %d", (int)size);
			fds[k] = (char *)malloc(size);
			memset(fds[k], 0, size);

			if (!m)	// full match, just substitute
				sprintf(fds[k], "/proc/self/fd/%d",
						input_fds[n++]);

			char *argv_end = NULL;
			while (m) {	// substring match
				DPRINTF("Matched: %s", m);
				char new_argv[size];
				char argv_start[size];
				char proc_fd[20];
				memset(new_argv, 0, size);
				memset(argv_start, 0, size);
				memset(proc_fd, 0, 20);

				sprintf(proc_fd, "/proc/self/fd/%d",
						input_fds[n++]);
				DPRINTF("proc_fd: %s", proc_fd);
				if (!argv_end)
					strncpy(argv_start, exec_argv[k],
							m - exec_argv[k]);
				else
					strncpy(argv_start, argv_end,
							m - argv_end);
				DPRINTF("argv_start: %s\n", argv_start);
				argv_end = m + 2;
				DPRINTF("argv_end: %s\n", argv_end);
				if (strlen(fds[k]) > 0) {
					strcpy(new_argv, fds[k]);
					sprintf(fds[k], "%s%s%s", new_argv,
							argv_start, proc_fd);
				} else
					sprintf(fds[k], "%s%s", argv_start,
							proc_fd);
				m = strstr(argv_end, "<|");
				if (!m) {
					strcpy(new_argv, fds[k]);
					sprintf(fds[k], "%s%s",
						new_argv, argv_end);
				}
				DPRINTF("fds[k]: %s\n", fds[k]);
			}
			exec_argv[k] = fds[k];
		}
		DPRINTF("After sub exec_argv[%d]: %s\n", k, exec_argv[k]);
	}

	// Execute command
	if (exec_argv[0][0] == '/')
		execv(guest_program_name, exec_argv);
	else
		execvp(guest_program_name, exec_argv);

	if (ninputs)
		free(ninputs);
	if (noutputs)
		free(noutputs);

	if (input_fds)
		free(input_fds);

	for (k = 0; k < argc - 2; k++)
		if (fds[k])
			free(fds[k]);

	return 0;
}