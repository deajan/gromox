// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 grommunio GmbH
// This file is part of Gromox.
#define _GNU_SOURCE 1
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mysql.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include <libHX/io.h>
#include <gromox/config_file.hpp>
#include <gromox/database_mysql.hpp>
#include <gromox/endian.hpp>
#include <gromox/exmdb_client.hpp>
#include <gromox/exmdb_rpc.hpp>
#include <gromox/ext_buffer.hpp>
#include <gromox/fileio.h>
#include <gromox/list_file.hpp>
#include <gromox/mapidefs.h>
#include <gromox/paths.h>
#include <gromox/pcl.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/scope.hpp>
#include <gromox/tie.hpp>
#include <gromox/util.hpp>
#include "genimport.hpp"

using namespace std::string_literals;
using namespace gromox;
namespace exmdb_client = exmdb_client_remote;

static std::string g_dstuser;
static std::string g_storedir_s;
const char *g_storedir;
unsigned int g_user_id, g_show_tree, g_show_props, g_wet_run = 1, g_public_folder;

YError::YError(const std::string &s) : m_str(s)
{}

YError::YError(std::string &&s) : m_str(std::move(s))
{}

YError::YError(const char *fmt, ...)
{
	if (strchr(fmt, '%') == nullptr) {
		m_str = fmt;
		return;
	}
	va_list args;
	va_start(args, fmt);
	std::unique_ptr<char[], gi_delete> strp;
	auto ret = vasprintf(&unique_tie(strp), fmt, args);
	va_end(args);
	m_str = ret >= 0 && strp != nullptr ? strp.get() : "vasprintf";
}

void tree(unsigned int depth)
{
	if (!g_show_tree)
		return;
	fprintf(stderr, "%-*s \\_ ", depth * 4, "");
}

void tlog(const char *fmt, ...)
{
	if (!g_show_tree)
		return;
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static void gi_dump_tpropval(unsigned int depth, const TAGGED_PROPVAL &tp)
{
	if (g_show_props)
		tree(depth);
	auto s = tp.value_repr(g_show_props);
	if (s.empty())
		fprintf(stderr, "PG-1130: unsupported proptype %xh\n", tp.proptag);
	tlog("%08xh:%s%s", tp.proptag, s.c_str(), g_show_props ? "\n" : ", ");
}

void gi_dump_tpropval_a(unsigned int depth, const TPROPVAL_ARRAY &props)
{
	if (props.count == 0)
		return;
	tree(depth);
	tlog("props(%d):", props.count);
	tlog(g_show_props ? "\n" : " {");
	for (size_t i = 0; i < props.count; ++i)
		gi_dump_tpropval(depth + 1, props.ppropval[i]);
	if (!g_show_props)
		tlog("}\n");
	auto p = props.get<const char>(PR_DISPLAY_NAME);
	if (p != nullptr) {
		tree(depth);
		tlog("display_name=\"%s\"\n", p);
	}
	p = props.get<char>(PR_SUBJECT);
	if (p != nullptr) {
		tree(depth);
		tlog("subject=\"%s\"\n", p);
	}
	p = props.get<char>(PR_ATTACH_LONG_FILENAME);
	if (p != nullptr) {
		tree(depth);
		tlog("filename=\"%s\"\n", p);
	}
}

void gi_dump_tarray_set(unsigned int depth, const tarray_set &tset)
{
	for (size_t i = 0; i < tset.count; ++i) {
		tree(depth);
		tlog("set %zu\n", i);
		gi_dump_tpropval_a(depth + 1, *tset.pparray[i]);
	}
}

void gi_dump_msgctnt(unsigned int depth, const MESSAGE_CONTENT &ctnt)
{
	gi_dump_tpropval_a(depth, ctnt.proplist);
	auto &r = ctnt.children.prcpts;
	if (r != nullptr) {
		for (size_t n = 0; n < r->count; ++n) {
			tree(depth);
			tlog("Recipient #%zu\n", n);
			if (r->pparray[n] != nullptr)
				gi_dump_tpropval_a(depth + 1, *r->pparray[n]);
		}
	}
	auto &a = ctnt.children.pattachments;
	if (a != nullptr) {
		for (size_t n = 0; n < a->count; ++n) {
			tree(depth);
			tlog("Attachment #%zu\n", n);
			auto atc = a->pplist[n];
			if (atc == nullptr)
				continue;
			gi_dump_tpropval_a(depth + 1, atc->proplist);
			if (atc->pembedded == nullptr)
				continue;
			tree(depth + 1);
			tlog("Embedded message\n");
			gi_dump_msgctnt(depth + 2, *atc->pembedded);
		}
	}
}

void gi_dump_folder_map(const gi_folder_map_t &map)
{
	if (!g_show_props)
		return;
	fprintf(stderr, "Folder map (%zu entries):\n", map.size());
	fprintf(stderr, "\t# HierID (hex) -> Target name\n");
	for (const auto &[nid, tgt] : map)
		fprintf(stderr, "\t%xh -> %s (%s %llxh)\n", nid, tgt.create_name.c_str(),
		        tgt.create ? "create under" : "splice into",
		        static_cast<unsigned long long>(tgt.fid_to));
}

void gi_dump_name_map(const gi_name_map &map)
{
	if (!g_show_props)
		return;
	fprintf(stderr, "Named properties (%zu entries):\n", map.size());
	fprintf(stderr, "\t# PROPID (hex) <-> MAPINAMEID definition:\n");
	for (const auto &[propid, propname] : map) {
		char g[40];
		propname.guid.to_str(g, std::size(g), 38);
		if (propname.kind == MNID_ID)
			fprintf(stderr, "\t%08xh <-> {MNID_ID, %s, %xh}\n",
				propid, g, static_cast<unsigned int>(propname.lid));
		else if (propname.kind == MNID_STRING)
			fprintf(stderr, "\t%08xh <-> {MNID_STRING, %s, %s}\n",
				propid, g, propname.name.c_str());
	}
}

void gi_folder_map_read(const void *buf, size_t bufsize, gi_folder_map_t &map)
{
	EXT_PULL ep;
	ep.init(buf, bufsize, zalloc, EXT_FLAG_WCOUNT);
	uint64_t max = 0;
	if (ep.g_uint64(&max) != EXT_ERR_SUCCESS)
		throw YError("PG-1100");
	for (size_t n = 0; n < max; ++n) {
		uint32_t nid;
		uint8_t create;
		uint64_t fidto;
		std::unique_ptr<char[], gi_delete> name;
		if (ep.g_uint32(&nid) != EXT_ERR_SUCCESS ||
		    ep.g_uint8(&create) != EXT_ERR_SUCCESS ||
		    ep.g_uint64(&fidto) != EXT_ERR_SUCCESS ||
		    ep.g_str(&unique_tie(name)) != EXT_ERR_SUCCESS)
			throw YError("PG-1101");
		map.insert_or_assign(nid, tgt_folder{static_cast<bool>(create), fidto, name != nullptr ? name.get() : ""});
	}
}

void gi_folder_map_write(const gi_folder_map_t &map)
{
	EXT_PUSH ep;
	if (!ep.init(nullptr, 0, EXT_FLAG_WCOUNT))
		throw std::bad_alloc();
	if (ep.p_uint64(map.size()) != EXT_ERR_SUCCESS)
		throw YError("PG-1102");
	for (const auto &[nid, tgt] : map)
		if (ep.p_uint32(nid) != EXT_ERR_SUCCESS ||
		    ep.p_uint8(!!tgt.create) != EXT_ERR_SUCCESS ||
		    ep.p_uint64(tgt.fid_to) != EXT_ERR_SUCCESS ||
		    ep.p_str(tgt.create_name.c_str()) != EXT_ERR_SUCCESS)
			throw YError("PG-1103");
	uint64_t xsize = cpu_to_le64(ep.m_offset);
	auto ret = HXio_fullwrite(STDOUT_FILENO, &xsize, sizeof(xsize));
	if (ret < 0)
		throw YError("PG-1104: %s", strerror(-ret));
	ret = HXio_fullwrite(STDOUT_FILENO, ep.m_vdata, ep.m_offset);
	if (ret < 0)
		throw YError("PG-1106: %s", strerror(-ret));
}

void gi_name_map_read(const void *buf, size_t bufsize, gi_name_map &map)
{
	EXT_PULL ep;
	ep.init(buf, bufsize, zalloc, EXT_FLAG_WCOUNT);
	uint64_t max = 0;
	if (ep.g_uint64(&max) != EXT_ERR_SUCCESS)
		throw YError("PG-1108");
	for (size_t n = 0; n < max; ++n) {
		uint32_t proptag;
		PROPERTY_NAME propname{};
		if (ep.g_uint32(&proptag) != EXT_ERR_SUCCESS ||
		    ep.g_propname(&propname) != EXT_ERR_SUCCESS)
			throw YError("PG-1109");
		try {
			map.insert_or_assign(proptag, propname);
		} catch (const std::bad_alloc &) {
			free(propname.pname);
			throw;
		}
		free(propname.pname);
	}
}

void gi_name_map_write(const gi_name_map &map)
{
	EXT_PUSH ep;
	if (!ep.init(nullptr, 0, EXT_FLAG_WCOUNT))
		throw std::bad_alloc();
	if (ep.p_uint64(map.size()) != EXT_ERR_SUCCESS)
		throw YError("PG-1110");
	for (const auto &[propid, xn] : map)
		if (ep.p_uint32(propid) != EXT_ERR_SUCCESS ||
		    ep.p_propname(static_cast<PROPERTY_NAME>(xn)) != EXT_ERR_SUCCESS)
			throw YError("PG-1111");
	uint64_t xsize = cpu_to_le64(ep.m_offset);
	auto ret = HXio_fullwrite(STDOUT_FILENO, &xsize, sizeof(xsize));
	if (ret < 0)
		throw YError("PG-1112: %s", strerror(-ret));
	ret = HXio_fullwrite(STDOUT_FILENO, ep.m_vdata, ep.m_offset);
	if (ret < 0)
		throw YError("PG-1114: %s", strerror(-ret));
}

uint16_t gi_resolve_namedprop(const PROPERTY_XNAME &xpn_req)
{
	PROPERTY_NAME pn_req(xpn_req);
	PROPNAME_ARRAY pna_req;
	pna_req.count = 1;
	pna_req.ppropname = &pn_req;

	PROPID_ARRAY pid_rsp{};
	if (!exmdb_client::get_named_propids(g_storedir, TRUE, &pna_req, &pid_rsp))
		throw YError("PF-1047: request to server for propname mapping failed");
	if (pid_rsp.count != 1)
		throw YError("PF-1048");
	return pid_rsp.ppropid[0];
}

int exm_set_change_keys(TPROPVAL_ARRAY *props, uint64_t change_num)
{
	/* Set the change key and initial PCL for the object */
	XID zxid{g_public_folder ? rop_util_make_domain_guid(g_user_id) :
	         rop_util_make_user_guid(g_user_id), change_num};
	char tmp_buff[22];
	BINARY bxid;
	EXT_PUSH ep;
	if (!ep.init(tmp_buff, std::size(tmp_buff), 0) ||
	    ep.p_xid(zxid) != EXT_ERR_SUCCESS) {
		fprintf(stderr, "exm: ext_push: ENOMEM\n");
		return -ENOMEM;
	}
	bxid.pv = tmp_buff;
	bxid.cb = ep.m_offset;
	PCL pcl;
	if (!pcl.append(zxid)) {
		fprintf(stderr, "exm: pcl_append: ENOMEM\n");
		return -ENOMEM;
	}
	std::unique_ptr<BINARY, gi_delete> pclbin(pcl.serialize());
	if (pclbin == nullptr){
		fprintf(stderr, "exm: pcl_serialize: ENOMEM\n");
		return -ENOMEM;
	}
	int ret;
	if ((ret = props->set(PidTagChangeNumber, &change_num)) != 0 ||
	    (ret = props->set(PR_CHANGE_KEY, &bxid)) != 0 ||
	    (ret = props->set(PR_PREDECESSOR_CHANGE_LIST, pclbin.get())) != 0) {
		fprintf(stderr, "%s: %s\n", __func__, strerror(-ret));
		return ret;
	}
	return 0;
}

/**
 * @o_excl:	Enforce that we are the first to create the folder, just like
 * 		open(2)'s %O_EXCL flag.
 */
int exm_create_folder(uint64_t parent_fld, TPROPVAL_ARRAY *props, bool o_excl,
    uint64_t *new_fld_id)
{
	uint64_t change_num = 0;
	if (!exmdb_client::allocate_cn(g_storedir, &change_num)) {
		fprintf(stderr, "exm: allocate_cn(fld) RPC failed\n");
		return -EIO;
	}
	if (!props->has(PR_LAST_MODIFICATION_TIME)) {
		auto last_time = rop_util_current_nttime();
		auto ret = props->set(PR_LAST_MODIFICATION_TIME, &last_time);
		if (ret != 0)
			return ret;
	}
	int ret;
	if ((ret = props->set(PidTagParentFolderId, &parent_fld)) != 0 ||
	    (ret = exm_set_change_keys(props, change_num)) != 0) {
		fprintf(stderr, "exm: tpropval: %s\n", strerror(-ret));
		return ret;
	}
	auto dn = props->get<const char>(PR_DISPLAY_NAME);
	if (!o_excl && dn != nullptr) {
		if (!exmdb_client::get_folder_by_name(g_storedir,
		    parent_fld, dn, new_fld_id)) {
			fprintf(stderr, "exm: get_folder_by_name \"%s\" RPC/network failed\n", dn);
			return -EIO;
		}
		if (*new_fld_id != 0)
			return 0;
	}
	if (dn == nullptr)
		dn = "";
	if (!exmdb_client::create_folder_by_properties(g_storedir, CP_ACP,
	    props, new_fld_id)) {
		fprintf(stderr, "exm: create_folder_by_properties \"%s\" RPC failed\n", dn);
		return -EIO;
	}
	if (*new_fld_id == 0) {
		fprintf(stderr, "exm: Could not create folder \"%s\". "
			"Either it already existed or some there was some other unspecified problem.\n", dn);
		return -EEXIST;
	}
	return 0;
}

int exm_permissions(eid_t fid, const std::vector<PERMISSION_DATA> &perms)
{
	if (perms.size() == 0)
		return 0;
	if (!exmdb_client::update_folder_permission(g_storedir, fid, false,
	    perms.size(), perms.data())) {
		fprintf(stderr, "exm: update_folder_perm(%llxh) RPC failed\n",
		        static_cast<unsigned long long>(fid));
		return -EIO;
	}
	return 0;
}

int exm_deliver_msg(const char *target, MESSAGE_CONTENT *ct, unsigned int mode)
{
	ct->proplist.erase(PidTagChangeNumber);
	auto ts = rop_util_current_nttime();
	if (ct->proplist.set(PR_MESSAGE_DELIVERY_TIME, &ts) != 0)
		/* ignore */;
	uint64_t folder_id = 0, msg_id = 0;
	uint32_t r32 = 0;
	if (mode & DELIVERY_TWOSTEP)
		mode &= ~(DELIVERY_DO_RULES | DELIVERY_DO_NOTIF);
	if (!exmdb_client::deliver_message(g_storedir, ENVELOPE_FROM_NULL,
	    target, CP_ACP, mode, ct, "", &folder_id, &msg_id, &r32)) {
		fprintf(stderr, "exm: deliver_message RPC failed: code %u\n",
		        r32);
		return -EIO;
	}
	auto dm_status = static_cast<deliver_message_result>(r32);
	switch (dm_status) {
	case deliver_message_result::result_ok:
		break;
	case deliver_message_result::result_error:
		fprintf(stderr, "Message rejected - unspecified reason\n");
		return EXIT_FAILURE;
	case deliver_message_result::mailbox_full_bysize:
		fprintf(stderr, "Message rejected - mailbox has reached quota limit");
		return EXIT_FAILURE;
	case deliver_message_result::mailbox_full_bymsg:
		fprintf(stderr, "Message rejected - mailbox has reached maximum message count (cf. exmdb_provider.cfg:max_store_message_count)");
		return EXIT_FAILURE;
	}
	if (!(mode & DELIVERY_TWOSTEP))
		return EXIT_SUCCESS;
	fprintf(stderr, "Exercising TWOSTEP ruleprocessor:\n");
	if (msg_id == 0) {
		fprintf(stderr, "deliver_message RPC did not give us a message_id -- not executing any rules.\n");
		return EXIT_SUCCESS;
	}
	auto err = exmdb_local_rules_execute(g_storedir, ENVELOPE_FROM_NULL,
	           target, folder_id, msg_id);
	if (err != ecSuccess) {
		fprintf(stderr, "Rule execution not successful: %s\n", mapi_strerror(err));
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int exm_create_msg(uint64_t parent_fld, MESSAGE_CONTENT *ctnt)
{
	uint64_t msg_id = 0, change_num = 0;
	if (!exmdb_client::allocate_message_id(g_storedir, parent_fld, &msg_id)) {
		fprintf(stderr, "exm: allocate_message_id RPC failed (timeout?)\n");
		return -EIO;
	} else if (!exmdb_client::allocate_cn(g_storedir, &change_num)) {
		fprintf(stderr, "exm: allocate_cn(msg) RPC failed\n");
		return -EIO;
	}

	XID zxid{g_public_folder ? rop_util_make_domain_guid(g_user_id) :
	         rop_util_make_user_guid(g_user_id), change_num};
	char tmp_buff[22];
	BINARY bxid;
	EXT_PUSH ep;
	if (!ep.init(tmp_buff, std::size(tmp_buff), 0) ||
	    ep.p_xid(zxid) != EXT_ERR_SUCCESS) {
		fprintf(stderr, "exm: ext_push: ENOMEM\n");
		return -ENOMEM;
	}
	bxid.pv = tmp_buff;
	bxid.cb = ep.m_offset;
	PCL pcl;
	if (!pcl.append(zxid)) {
		fprintf(stderr, "exm: pcl_append: ENOMEM\n");
		return -ENOMEM;
	}
	std::unique_ptr<BINARY, gi_delete> pclbin(pcl.serialize());
	if (pclbin == nullptr){
		fprintf(stderr, "exm: pcl_serialize: ENOMEM\n");
		return -ENOMEM;
	}
	auto props = &ctnt->proplist;
	if (!props->has(PR_LAST_MODIFICATION_TIME)) {
		auto last_time = rop_util_current_nttime();
		auto ret = props->set(PR_LAST_MODIFICATION_TIME, &last_time);
		if (ret != 0)
			return ret;
	}
	int ret;
	if ((ret = props->set(PidTagMid, &msg_id)) != 0 ||
	    (ret = props->set(PidTagChangeNumber, &change_num)) != 0 ||
	    (ret = props->set(PR_CHANGE_KEY, &bxid)) != 0 ||
	    (ret = props->set(PR_PREDECESSOR_CHANGE_LIST, pclbin.get())) != 0) {
		fprintf(stderr, "exm: tpropval: %s\n", strerror(-ret));
		return ret;
	}
	ec_error_t e_result = ecRpcFailed;
	if (!exmdb_client::write_message(g_storedir, g_dstuser.c_str(), CP_UTF8,
	    parent_fld, ctnt, &e_result)) {
		fprintf(stderr, "exm: write_message RPC failed\n");
		return -EIO;
	} else if (e_result != ecSuccess) {
		fprintf(stderr, "exm: write_message: %s\n", mapi_strerror(e_result));
		return -EIO;
	}
	return 0;
}

static std::string sql_escape(MYSQL *sqh, const char *in)
{
	std::string out;
	out.resize(strlen(in) * 2 + 1);
	auto ret = mysql_real_escape_string(sqh, out.data(), in, strlen(in));
	out.resize(ret);
	return out;
}

static std::unique_ptr<MYSQL, mysql_delete> sql_login()
{
	auto cfg = config_file_initd("mysql_adaptor.cfg", PKGSYSCONFDIR, nullptr);
	if (cfg == nullptr) {
		fprintf(stderr, "exm: No mysql_adaptor.cfg: %s\n", strerror(errno));
		return nullptr;
	}
	auto sql_host = cfg->get_value("mysql_host");
	auto v = cfg->get_value("mysql_port");
	auto sql_port = v != nullptr ? strtoul(v, nullptr, 0) : 0;
	auto sql_user = cfg->get_value("mysql_username");
	if (sql_user == nullptr)
		sql_user = "root";
	auto sql_pass = cfg->get_value("mysql_password");
	auto sql_dbname = cfg->get_value("mysql_dbname");
	if (sql_dbname == nullptr)
		sql_dbname = "email";
	std::unique_ptr<MYSQL, mysql_delete> conn(mysql_init(nullptr));
	if (conn == nullptr) {
		fprintf(stderr, "exm: mysql_init failed\n");
		return nullptr;
	}
	if (mysql_real_connect(conn.get(), sql_host, sql_user, sql_pass,
	    sql_dbname, sql_port, nullptr, 0) == nullptr) {
		fprintf(stderr, "exm: Failed to connect to SQL %s@%s: %s\n",
		        sql_user, sql_host, mysql_error(conn.get()));
		return nullptr;
	}
	if (mysql_set_character_set(conn.get(), "utf8mb4") != 0) {
		fprintf(stderr, "mysql: \"utf8mb4\" not available: %s\n",
		        mysql_error(conn.get()));
		return nullptr;
	}
	return conn;
}

static int sql_meta(MYSQL *sqh, const char *username, bool is_domain,
    unsigned int *user_id, std::string &storedir) try
{
	auto query = is_domain ?
		("SELECT `id`, `homedir` FROM `domains` WHERE `domainname`='"s + sql_escape(sqh, username) + "'") :
		("SELECT `id`, `maildir` FROM `users` WHERE `username`='"s + sql_escape(sqh, username) + "'");
	if (mysql_real_query(sqh, query.c_str(), query.size()) != 0) {
		fprintf(stderr, "exm: mysql_query: %s\n", mysql_error(sqh));
		return -EINVAL;
	}
	DB_RESULT result = mysql_store_result(sqh);
	if (result == nullptr) {
		fprintf(stderr, "exm: mysql_store: %s\n", mysql_error(sqh));
		return -ENOENT;
	}
	auto row = result.fetch_row();
	if (row == nullptr)
		return -ENOENT;
	*user_id = strtoul(row[0], nullptr, 0);
	storedir = znul(row[1]);
	return 0;
} catch (const std::bad_alloc &) {
	return -ENOMEM;
}

static int sql_dir_to_user(MYSQL *sqh, const char *dir,
    unsigned int &user_id, std::string &username) try
{
	auto query = "SELECT `id`, `username` FROM `users` WHERE `maildir`='"s + sql_escape(sqh, dir) + "'";
	if (mysql_real_query(sqh, query.c_str(), query.size()) != 0) {
		fprintf(stderr, "exm: mysql_query: %s\n", mysql_error(sqh));
		return -EINVAL;
	}
	DB_RESULT result = mysql_store_result(sqh);
	if (result == nullptr) {
		fprintf(stderr, "exm: mysql_store: %s\n", mysql_error(sqh));
		return -ENOENT;
	}
	auto row = result.fetch_row();
	if (row == nullptr || row[0] == nullptr || row[1] == nullptr)
		return -ENOENT;
	user_id = strtoul(row[0], nullptr, 0);
	username = row[1];
	return 0;
} catch (const std::bad_alloc &) {
	return -ENOMEM;
}

void gi_setup_early(const char *username)
{
	if (*username == '@') {
		g_public_folder = true;
		++username;
	}
	g_dstuser = username;
}

int gi_setup_from_dir()
{
	auto sqh = sql_login();
	if (sqh == nullptr)
		return EXIT_FAILURE;
	auto ret = sql_dir_to_user(sqh.get(), g_storedir, g_user_id, g_dstuser);
	if (ret == -ENOENT) {
		fprintf(stderr, "exm: No user with homedir \"%s\"\n", g_storedir);
		fprintf(stderr, "exm: (No attempt was made to locate a domain mailbox)\n");
		return EXIT_FAILURE;
	} else if (ret < 0) {
		fprintf(stderr, "get_id_from_maildir(\"%s\"): %s\n",
		        g_storedir, strerror(-ret));
		return EXIT_FAILURE;
	}
	exmdb_client_init(1, 0);
	return exmdb_client_run(PKGSYSCONFDIR);
}

int gi_setup()
{
	auto sqh = sql_login();
	if (sqh == nullptr)
		return EXIT_FAILURE;
	auto ret = sql_meta(sqh.get(), g_dstuser.c_str(), g_public_folder,
	           &g_user_id, g_storedir_s);
	sqh.reset();
	if (ret == -ENOENT) {
		fprintf(stderr, "exm: No such %s \"%s\"\n",
		        g_public_folder ? "domain" : "username", g_dstuser.c_str());
		return EXIT_FAILURE;
	} else if (ret < 0) {
		fprintf(stderr, "exm: sql_meta(\"%s\"): %s\n",
		        g_dstuser.c_str(), strerror(-ret));
		return EXIT_FAILURE;
	}
	g_storedir = g_storedir_s.c_str();
	exmdb_client_init(1, 0);
	return exmdb_client_run(PKGSYSCONFDIR);
}

void gi_shutdown()
{
	exmdb_client_stop();
}
