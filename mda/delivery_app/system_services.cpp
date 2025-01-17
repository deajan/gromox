// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstdio>
#include <gromox/svc_loader.hpp>
#include <gromox/util.hpp>
#include "delivery.hpp"

using namespace gromox;
int (*system_services_check_domain)(const char *);

int system_services_run()
{
#define E(f, s) do { \
	(f) = reinterpret_cast<decltype(f)>(service_query((s), "system", typeid(*(f)))); \
	if ((f) == nullptr) { \
		mlog(LV_ERR, "system_services: failed to get the \"%s\" service", (s)); \
		return -1; \
	} \
} while (false)
	E(system_services_check_domain, "domain_list_query");
	return 0;
#undef E
}

void system_services_stop()
{
	service_release("domain_list_query", "system");
}
