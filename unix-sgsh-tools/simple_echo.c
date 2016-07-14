#include <stdio.h>		/* printf() */
#include <stdlib.h>		/* exit() */

#include "sgsh-negotiate.h"

int
main(int argc, char *argv[])
{
	int ninputfds, noutputfds;
	int *inputfds, *outputfds;

	if (sgsh_negotiate("secho", NULL, NULL, NULL, NULL) != 0)
		exit(1);

	++argv;
	while (*argv) {
		(void)printf("%s", *argv);
		if (*++argv)
			putchar(' ');
	putchar('\n');

	return 0;
}