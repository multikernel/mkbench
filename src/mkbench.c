#include "common.h"

#include <stdio.h>
#include <string.h>

static const struct {
	const char *name;
	int (*fn)(int argc, char **argv);
	const char *help;
} cmds[] = {
	{ "check",    check_main,    "-c CPU -m NODE            verify pinning and NUMA binding" },
	{ "memlat",   memlat_main,   "-c CPU -m NODE -s SIZE    pointer-chase load latency" },
	{ "membw",    membw_main,    "-c LIST -m NODE -s SIZE -k read|write|copy|triad" },
	{ "pingpong", pingpong_main, "-a CPU -b CPU -k rtt|false-shared|false-padded" },
	{ "atomics",  atomics_main,  "-c LIST                   contended fetch_add" },
	{ "lock",     lock_main,     "-c LIST -k mutex|spin     lock handoff/contention" },
	{ "ipc",      ipc_main,      "-a CPU -b CPU -k ring|ringwait|pipe|unix|tcp [-T]" },
	{ "wakeup",   wakeup_main,   "-a CPU -b CPU             sleeping futex wake latency" },
};

static void usage(void)
{
	fprintf(stderr, "usage: mkbench <command> [options]\n\ncommands:\n");
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		fprintf(stderr, "  %-9s %s\n", cmds[i].name, cmds[i].help);
	fprintf(stderr, "  %-9s %s\n", "csvheader", "print the CSV header line");
	fprintf(stderr, "\ncommon options: -r repeats  -l tier-label  -n iterations\n");
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage();
		return 1;
	}
	if (!strcmp(argv[1], "csvheader")) {
		csv_header();
		return 0;
	}
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		if (!strcmp(argv[1], cmds[i].name))
			return cmds[i].fn(argc - 1, argv + 1);
	usage();
	return 1;
}
