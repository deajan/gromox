// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 grammm GmbH
// This file is part of Gromox.
#define DECLARE_HOOK_API_STATIC
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <libHX/string.h>
#include <gromox/config_file.hpp>
#include <gromox/database_mysql.hpp>
#include <gromox/hook_common.h>
#include <gromox/mem_file.hpp>
#include <gromox/scope.hpp>
#include "../exch/mysql_adaptor/mysql_adaptor.h"

using namespace gromox;

static std::atomic<bool> xa_notify_stop{false};
static std::condition_variable xa_thread_wake;
static std::map<std::string, std::string> xa_alias_map;
static std::mutex xa_alias_lock;
static std::thread xa_thread;
static mysql_adaptor_init_param g_parm;
static std::chrono::seconds g_cache_lifetime;

static MYSQL *sql_make_conn()
{
	auto conn = mysql_init(nullptr);
	if (conn == nullptr)
		return nullptr;
	if (g_parm.timeout > 0) {
		mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &g_parm.timeout);
		mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &g_parm.timeout);
	}
	if (mysql_real_connect(conn, g_parm.host.c_str(), g_parm.user.c_str(),
	    g_parm.pass.size() != 0 ? g_parm.pass.c_str() : nullptr,
	    g_parm.dbname.c_str(), g_parm.port, nullptr, 0) == nullptr) {
		printf("[mysql_adaptor]: Failed to connect to mysql server: %s\n",
		       mysql_error(conn));
		mysql_close(conn);
		return nullptr;
	}
	if (mysql_set_character_set(conn, "utf8mb4") != 0) {
		fprintf(stderr, "[mysql_adaptor]: \"utf8mb4\" not available: %s\n",
		        mysql_error(conn));
		mysql_close(conn);
		return nullptr;
	}
	return conn;
}

static const std::string &xa_alias_lookup(const std::string &srch)
{
	static const std::string empty;
	std::lock_guard hold(xa_alias_lock);
	auto i = xa_alias_map.find(srch);
	return i != xa_alias_map.cend() ? i->second : empty;
}

static void xa_refresh_aliases(MYSQL *conn) try
{
	static const char query[] = "SELECT aliasname, mainname FROM aliases";
	if (mysql_query(conn, query) != 0)
		return;
	DB_RESULT res = mysql_store_result(conn);
	decltype(xa_alias_map) newmap;
	DB_ROW row;
	while ((row = res.fetch_row()) != nullptr)
		if (row[0] != nullptr && row[1] != nullptr)
			newmap.emplace(row[0], row[1]);
	std::lock_guard hold(xa_alias_lock);
	std::swap(xa_alias_map, newmap);
	fprintf(stderr, "[alias_resolve]: I-1612: refreshed alias map (%zu entries)\n",
	        xa_alias_map.size());
} catch (const std::bad_alloc &) {
}

static void xa_refresh_thread()
{
	std::mutex slp_mtx;
	{
		auto conn = sql_make_conn();
		std::unique_lock slp_hold(slp_mtx);
		xa_refresh_aliases(conn);
	}
	while (!xa_notify_stop) {
		std::unique_lock slp_hold(slp_mtx);
		xa_thread_wake.wait_for(slp_hold, g_cache_lifetime);
		if (xa_notify_stop)
			break;
		auto conn = sql_make_conn();
		xa_refresh_aliases(conn);
	}
}

static BOOL xa_alias_subst(MESSAGE_CONTEXT *ctx) try
{
	auto ctrl = ctx->pcontrol;
	if (ctrl->bound_type >= BOUND_SELF)
		return false;

	MEM_FILE temp_file, rcpt_file;
	mem_file_init(&temp_file, ctrl->f_rcpt_to.allocator);
	auto cl_0 = make_scope_exit([&]() { mem_file_free(&temp_file); });
	mem_file_init(&rcpt_file, ctrl->f_rcpt_to.allocator);
	auto cl_1 = make_scope_exit([&]() { mem_file_free(&rcpt_file); });
	ctrl->f_rcpt_to.copy_to(rcpt_file);

	if (strchr(ctrl->from, '@') != nullptr) {
		auto repl = xa_alias_lookup(ctrl->from);
		if (repl.size() > 0) {
			log_info(6, "alias_resolve: subst FROM %s -> %s", ctrl->from, repl.c_str());
			gx_strlcpy(ctrl->from, repl.c_str(), arsizeof(ctrl->from));
		}
	}

	bool replaced = false;
	char rcpt_to[UADDR_SIZE];
	while (rcpt_file.readline(rcpt_to, arsizeof(rcpt_to)) != MEM_END_OF_FILE) {
		if (strchr(rcpt_to, '@') == nullptr) {
			temp_file.writeline(rcpt_to);
			continue;
		}
		auto repl = xa_alias_lookup(rcpt_to);
		if (repl.size() == 0) {
			temp_file.writeline(rcpt_to);
			continue;
		}
		log_info(6, "alias_resolve: subst RCPT %s -> %s", rcpt_to, repl.c_str());
		replaced = true;
		temp_file.writeline(repl.c_str());
	}
	if (replaced)
		temp_file.copy_to(ctrl->f_rcpt_to);
	return false;
} catch (const std::bad_alloc &) {
	log_info(5, "E-1611: ENOMEM\n");
	return false;
}

static constexpr const cfg_directive mysql_directives[] = {
	{"mysql_host", "localhost"},
	{"mysql_port", "3306"},
	{"mysql_username", "root"},
	{"mysql_password", ""},
	{"mysql_dbname", "email"},
	{"mysql_rdwr_timeout", "0", CFG_TIME},
	{},
};
static constexpr const cfg_directive xa_directives[] = {
	{"cache_lifetime", "1h", CFG_TIME},
	{},
};

static bool xa_reload_config(std::shared_ptr<CONFIG_FILE> mcfg, std::shared_ptr<CONFIG_FILE> acfg)
{
	if (mcfg == nullptr) {
		mcfg = config_file_initd("mysql_adaptor.cfg", get_config_path());
		if (mcfg != nullptr)
			config_file_apply(*mcfg, mysql_directives);
	}
	if (mcfg == nullptr) {
		printf("[mysql_adaptor]: config_file_initd mysql_adaptor.cfg: %s\n",
		       strerror(errno));
		return false;
	}
	g_parm.host = mcfg->get_value("mysql_host");
	g_parm.port = mcfg->get_ll("mysql_port");
	g_parm.user = mcfg->get_value("mysql_username");
	g_parm.pass = mcfg->get_value("mysql_password");
	g_parm.dbname = mcfg->get_value("mysql_dbname");
	g_parm.timeout = mcfg->get_ll("mysql_rdwr_timeout");
	printf("[alias_resolve]: mysql [%s]:%d, timeout=%d, db=%s\n",
	       g_parm.host.size() == 0 ? "*" : g_parm.host.c_str(), g_parm.port,
	       g_parm.timeout, g_parm.dbname.c_str());

	if (acfg == nullptr) {
		acfg = config_file_initd("alias_resolve.cfg", get_config_path());
		if (acfg != nullptr)
			config_file_apply(*acfg, xa_directives);
	}
	if (acfg == nullptr) {
		printf("[mysql_adaptor]: config_file_initd alias_resolve.cfg: %s\n",
		       strerror(errno));
		return false;
	}
	g_cache_lifetime = std::chrono::seconds(acfg->get_ll("cache_lifetime"));
	return true;
}

static BOOL xa_main(int reason, void **data)
{
	if (reason == PLUGIN_RELOAD) {
		xa_reload_config(nullptr, nullptr);
		xa_thread_wake.notify_one();
		return TRUE;
	}
	if (reason == PLUGIN_FREE) {
		xa_notify_stop = true;
		xa_thread_wake.notify_one();
		xa_thread.join();
		return TRUE;
	}
	if (reason != PLUGIN_INIT)
		return TRUE;
	LINK_HOOK_API(data);
	auto mcfg = config_file_initd("mysql_adaptor.cfg", get_config_path());
	if (mcfg == nullptr) {
		printf("[alias_resolve]: config_file_initd mysql_adaptor.cfg: %s\n",
		       strerror(errno));
		return false;
	}
	auto acfg = config_file_initd("alias_resolve.cfg", get_config_path());
	if (acfg == nullptr) {
		printf("[alias_resolve]: config_file_initd alias_resolve.cfg: %s\n",
		       strerror(errno));
		return false;
	}
	config_file_apply(*mcfg, mysql_directives);
	config_file_apply(*acfg, xa_directives);
	if (!xa_reload_config(mcfg, acfg) ||
	    !register_hook(xa_alias_subst))
		return false;
	try {
		xa_thread = std::thread(xa_refresh_thread);
	} catch (const std::system_error &e) {
		log_info(3, "alias_resolve: %s\n", e.what());
		return false;
	}
	return true;
}
HOOK_ENTRY(xa_main);