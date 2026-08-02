#include "common.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int check_main(int argc, char **argv)
{
	int cpu = 0, node = 0, opt, got_cpu, got_node;
	size_t sz = 16UL << 20;
	void *p;

	while ((opt = getopt(argc, argv, "c:m:")) != -1) {
		switch (opt) {
		case 'c': cpu = atoi(optarg); break;
		case 'm': node = atoi(optarg); break;
		default: return 1;
		}
	}

	pin_to_cpu(cpu);
	got_cpu = sched_getcpu();
	if (got_cpu != cpu) {
		printf("check FAIL: pinned to cpu %d but running on %d\n",
		       cpu, got_cpu);
		return 1;
	}

	p = alloc_pages_on(sz, node);
	touch_pages(p, sz);
	got_node = page_node(p);
	if (got_node != node) {
		printf("check FAIL: bound to node %d but page on %d\n",
		       node, got_node);
		return 1;
	}

	printf("check PASS cpu=%d node=%d\n", cpu, node);
	return 0;
}
