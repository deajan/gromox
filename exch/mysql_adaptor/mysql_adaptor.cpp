// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <mysql.h>
#include <set>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
#include <fmt/core.h>
#include <libHX/string.h>
#include <gromox/database_mysql.hpp>
#include <gromox/defs.h>
#include <gromox/icase.hpp>
#include <gromox/mysql_adaptor.hpp>
#include <gromox/util.hpp>
#include "sql2.hpp"

/**
 * @domain:	only users in the same domain may send to the mlist
 * @specified:	only allowed users may send to the mlist (tbl: "specifieds")
 */
enum class mlist_priv {
	all = 0, internal, domain, specified, outgoing,
};

#define MLIST_RESULT_OK					0
#define MLIST_RESULT_NONE				1
#define MLIST_RESULT_PRIVIL_DOMAIN		2
#define MLIST_RESULT_PRIVIL_INTERNAL	3
#define MLIST_RESULT_PRIVIL_SPECIFIED	4
#define JOIN_WITH_DISPLAYTYPE "LEFT JOIN user_properties AS dt ON u.id=dt.user_id AND dt.proptag=956628995" /* PR_DISPLAY_TYPE_EX */

/*
 * Terminology you might encounter in this file
 *
 * "maildir": a private store's location
 * "homedir": a public store's location
 */

using namespace std::string_literals;
using namespace gromox;

static std::mutex g_crypt_lock;

int mysql_adaptor_run()
{
	if (!db_upgrade_check())
		return -1;
	return 0;
}

void mysql_adaptor_stop()
{
	g_sqlconn_pool.clear();
}

errno_t mysql_adaptor_meta(const char *username, unsigned int wantpriv,
    sql_meta_result &mres) try
{
	char temp_name[UADDR_SIZE*2];

	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT u.password, dt.propval_str AS dtypx, u.address_status, "
		"u.privilege_bits, u.maildir, u.lang, u.externid, "
		"op1.value, op2.value, op3.value, op4.value, op5.value, op6.value, "
		"u.username FROM users AS u " JOIN_WITH_DISPLAYTYPE
		" LEFT JOIN domains AS d ON u.domain_id=d.id"
		" LEFT JOIN orgs ON d.org_id=orgs.id"
		" LEFT JOIN orgparam AS op1 ON orgs.id=op1.org_id AND op1.key='ldap_uri'"
		" LEFT JOIN orgparam AS op2 ON orgs.id=op2.org_id AND op2.key='ldap_binddn'"
		" LEFT JOIN orgparam AS op3 ON orgs.id=op3.org_id AND op3.key='ldap_bindpw'"
		" LEFT JOIN orgparam AS op4 ON orgs.id=op4.org_id AND op4.key='ldap_basedn'"
		" LEFT JOIN orgparam AS op5 ON orgs.id=op5.org_id AND op5.key='ldap_mail_attr'"
		" LEFT JOIN orgparam AS op6 ON orgs.id=op6.org_id AND op6.key='ldap_start_tls'"
		" WHERE u.username='"s + temp_name + "' OR u.altname='" + temp_name + "'"
		" LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return EIO;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr) {
		mres.errstr = "Could not store SQL result";
		return ENOMEM;
	}
	conn.finish();
	if (pmyres.num_rows() > 1) {
		mres.errstr = fmt::format("login \"{}\" is ambiguous", username);
		return ENOENT;
	} else if (pmyres.num_rows() != 1) {
		mres.errstr = fmt::format("user \"{}\" does not exist", username);
		return ENOENT;
	}
	
	auto myrow = pmyres.fetch_row();
	uint32_t dtypx;
	if (myrow[1] == nullptr) {
		mres.errstr = "PR_DISPLAY_TYPE_EX is missing for this user";
		return EINVAL;
	}
	dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
	if (dtypx != DT_MAILUSER && !(wantpriv & WANTPRIV_METAONLY)) {
		mres.errstr = "User is not a real user";
		return EACCES;
	}
	auto temp_status = strtoul(myrow[2], nullptr, 0);
	if (temp_status != 0 && !(wantpriv & WANTPRIV_METAONLY)) {
		auto uval = temp_status & AF_USER__MASK;
		if (temp_status & AF_DOMAIN__MASK) {
			mres.errstr = fmt::format("Domain of user \"{}\" is disabled!", username);
		} else if (uval == AF_USER_SHAREDMBOX) {
			mres.errstr = fmt::format("\"{}\" is a shared mailbox with no login", username);
		} else if (uval != 0) {
			mres.errstr = fmt::format("User \"{}\" is disabled", username);
		}
		return EACCES;
	}
	wantpriv &= ~WANTPRIV_METAONLY;

	auto allowedsvc = strtoul(myrow[3], nullptr, 0);
	if (wantpriv != 0 && !(allowedsvc & wantpriv)) {
		mres.errstr = fmt::format("\"{}\" is not authorized to use service(s) {:x}h",
		              username, wantpriv);
		return EACCES;
	}
	mres.maildir    = myrow[4];
	mres.lang       = znul(myrow[5]);
	mres.enc_passwd = myrow[0];
	mres.have_xid   = myrow[6] != nullptr;
	mres.ldap_uri    = znul(myrow[7]);
	mres.ldap_binddn = znul(myrow[8]);
	mres.ldap_bindpw = znul(myrow[9]);
	mres.ldap_basedn = znul(myrow[10]);
	mres.ldap_mail_attr = znul(myrow[11]);
	mres.ldap_start_tls = parse_bool(znul(myrow[12]));
	mres.username       = znul(myrow[13]);
	return 0;
} catch (const std::bad_alloc &e) {
	mlog(LV_ERR, "E-1701: ENOMEM");
	return ENOMEM;
} catch (const std::exception &e) {
	mlog(LV_ERR, "E-1701: %s", e.what());
	return EIO;
}

static BOOL firsttime_password(const char *username, const char *password,
    std::string &encrypt_passwd)
{
	std::unique_lock cr_hold(g_crypt_lock);
	encrypt_passwd = znul(crypt_wrapper(password));
	cr_hold.unlock();

	char temp_name[UADDR_SIZE*2];
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "UPDATE users SET password='"s + encrypt_passwd +
	            "' WHERE username='" + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	return TRUE;
}

static BOOL verify_password(const char *username, const char *password,
    const char *encrypt_passwd, std::string &errstr)
{
	std::unique_lock cr_hold(g_crypt_lock);
	if (strcmp(crypt_estar(password, encrypt_passwd), encrypt_passwd) == 0)
		return TRUE;
	cr_hold.unlock();
	errstr = "password error, please check it and retry";
	return FALSE;
}

BOOL mysql_adaptor_login2(const char *username, const char *password,
    std::string &encrypt_passwd, std::string &errstr) try
{
	BOOL ret;
	if (g_parm.enable_firsttimepw && encrypt_passwd.empty())
		ret = firsttime_password(username, password, encrypt_passwd);
	else
		ret = verify_password(username, password,
		      encrypt_passwd.c_str(), errstr);
	return ret;
} catch (const std::bad_alloc &) {
	mlog(LV_ERR, "E-1702: ENOMEM");
	return false;
}

BOOL mysql_adaptor_setpasswd(const char *username,
	const char *password, const char *new_password) try
{
	char temp_name[UADDR_SIZE*2];
	char encrypt_passwd[40];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT u.password, dt.propval_str AS dtypx, u.address_status, "
		"u.privilege_bits FROM users AS u " JOIN_WITH_DISPLAYTYPE
		" WHERE u.username='"s + temp_name + "' LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	auto dtypx = DT_MAILUSER;
	if (myrow[1] != nullptr)
		dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
	if (dtypx != DT_MAILUSER)
		return FALSE;
	auto temp_status = strtoul(myrow[2], nullptr, 0);
	if (0 != temp_status) {
		return FALSE;
	}
	if (!(strtoul(myrow[3], nullptr, 0) & USER_PRIVILEGE_CHGPASSWD))
		return FALSE;

	strncpy(encrypt_passwd, myrow[0], sizeof(encrypt_passwd));
	encrypt_passwd[sizeof(encrypt_passwd) - 1] = '\0';
	
	std::unique_lock cr_hold(g_crypt_lock);
	if (encrypt_passwd[0] != '\0' &&
	    strcmp(crypt_estar(password, encrypt_passwd), encrypt_passwd) != 0)
		return FALSE;
	gx_strlcpy(encrypt_passwd, crypt_wrapper(new_password), std::size(encrypt_passwd));
	cr_hold.unlock();
	qstr = "UPDATE users SET password='"s + encrypt_passwd +
	       "' WHERE username='" + temp_name + "'";
	if (!conn->query(qstr.c_str()))
		return false;
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1703", e.what());
	return false;
}

BOOL mysql_adaptor_get_username_from_id(unsigned int user_id,
    char *username, size_t ulen) try
{
	auto qstr = "SELECT username FROM users WHERE id=" + std::to_string(user_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	gx_strlcpy(username, myrow[0], ulen);
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1704", e.what());
	return false;
}

BOOL mysql_adaptor_get_id_from_username(const char *username, unsigned int *puser_id) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT id FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*puser_id = strtoul(myrow[0], nullptr, 0);
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1705", e.what());
	return false;
}

BOOL mysql_adaptor_get_id_from_maildir(const char *maildir, unsigned int *puser_id) try
{
	char temp_dir[512];
	
	mysql_adaptor_encode_squote(maildir, temp_dir);
	auto qstr =
		"SELECT u.id FROM users AS u " JOIN_WITH_DISPLAYTYPE
		" WHERE u.maildir='"s + temp_dir + "' AND dt.propval_str IN (0,7,8) LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*puser_id = strtoul(myrow[0], nullptr, 0);
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1706", e.what());
	return false;
}

bool mysql_adaptor_get_user_displayname(const char *username,
    char *pdisplayname, size_t dsize) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT u2.propval_str AS real_name, "
		"u3.propval_str AS nickname, dt.propval_str AS dtypx FROM users AS u "
		JOIN_WITH_DISPLAYTYPE " "
		"LEFT JOIN user_properties AS u2 ON u.id=u2.user_id AND u2.proptag=805371935 " /* PR_DISPLAY_NAME */
		"LEFT JOIN user_properties AS u3 ON u.id=u3.user_id AND u3.proptag=978255903 " /* PR_NICKNAME */
		"WHERE u.username='"s + temp_name + "' LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return false;
	auto myrow = pmyres.fetch_row();
	auto dtypx = DT_MAILUSER;
	if (myrow[2] != nullptr)
		dtypx = static_cast<enum display_type>(strtoul(myrow[2], nullptr, 0));
	gx_strlcpy(pdisplayname,
	       dtypx == DT_DISTLIST ? username :
	       myrow[0] != nullptr && *myrow[0] != '\0' ? myrow[0] :
	       myrow[1] != nullptr && *myrow[1] != '\0' ? myrow[1] :
	       username, dsize);
	return true;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1707", e.what());
	return false;
}

BOOL mysql_adaptor_get_user_privilege_bits(const char *username,
    uint32_t *pprivilege_bits) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT privilege_bits FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*pprivilege_bits = strtoul(myrow[0], nullptr, 0);
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1708", e.what());
	return false;
}

bool mysql_adaptor_get_user_lang(const char *username, char *lang, size_t lang_size) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT lang FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1) {
		lang[0] = '\0';	
	} else {
		auto myrow = pmyres.fetch_row();
		gx_strlcpy(lang, myrow[0], lang_size);
	}
	return true;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1709", e.what());
	return false;
}

BOOL mysql_adaptor_set_user_lang(const char *username, const char *lang) try
{
	char temp_name[UADDR_SIZE*2];
	std::string fq_string;
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "UPDATE users set lang='"s + lang +
		    "' WHERE username='" + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1710", e.what());
	return false;
}

bool mysql_adaptor_get_timezone(const char *username, char *zone, size_t zone_size) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT timezone FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1) {
		zone[0] = '\0';
	} else {
		auto myrow = pmyres.fetch_row();
		gx_strlcpy(zone, myrow[0], zone_size);
	}
	return true;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1712", e.what());
	return false;
}

BOOL mysql_adaptor_set_timezone(const char *username, const char *zone) try
{
	char temp_name[UADDR_SIZE*2];
	char temp_zone[128];
	
	mysql_adaptor_encode_squote(username, temp_name);
	mysql_adaptor_encode_squote(zone, temp_zone);
	auto qstr = "UPDATE users set timezone='"s + temp_zone +
	            "' WHERE username='" + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1713", e.what());
	return false;
}

bool mysql_adaptor_get_maildir(const char *username, char *maildir, size_t md_size) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT maildir FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	gx_strlcpy(maildir, myrow[0], md_size);
	return true;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1714", e.what());
	return false;
}

bool mysql_adaptor_get_homedir(const char *domainname, char *homedir, size_t dsize) try
{
	char temp_name[UDOM_SIZE*2];
	
	mysql_adaptor_encode_squote(domainname, temp_name);
	auto qstr = "SELECT homedir, domain_status FROM domains WHERE domainname='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return false;
	auto myrow = pmyres.fetch_row();
	gx_strlcpy(homedir, myrow[0], dsize);
	return true;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1716", e.what());
	return false;
}

bool mysql_adaptor_get_homedir_by_id(unsigned int domain_id, char *homedir,
    size_t dsize) try
{
	auto qstr = "SELECT homedir FROM domains WHERE id=" + std::to_string(domain_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return false;
	auto myrow = pmyres.fetch_row();
	gx_strlcpy(homedir, myrow[0], dsize);
	return true;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1717", e.what());
	return false;
}

BOOL mysql_adaptor_get_id_from_homedir(const char *homedir, unsigned int *pdomain_id) try
{
	char temp_dir[512];
	
	mysql_adaptor_encode_squote(homedir, temp_dir);
	auto qstr = "SELECT id FROM domains WHERE homedir='"s + temp_dir + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*pdomain_id = strtoul(myrow[0], nullptr, 0);
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1718", e.what());
	return false;
}

BOOL mysql_adaptor_get_user_ids(const char *username, unsigned int *puser_id,
    unsigned int *pdomain_id, enum display_type *dtypx) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT u.id, u.domain_id, dt.propval_str AS dtypx"
		" FROM users AS u " JOIN_WITH_DISPLAYTYPE
		" WHERE u.username='"s + temp_name + "' LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;	
	auto myrow = pmyres.fetch_row();
	*puser_id   = strtoul(myrow[0], nullptr, 0);
	*pdomain_id = strtoul(myrow[1], nullptr, 0);
	if (dtypx != nullptr) {
		*dtypx = DT_MAILUSER;
		if (myrow[2] != nullptr)
			*dtypx = static_cast<enum display_type>(strtoul(myrow[2], nullptr, 0));
	}
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1719", e.what());
	return false;
}

BOOL mysql_adaptor_get_domain_ids(const char *domainname,
    unsigned int *pdomain_id, unsigned int *porg_id) try
{
	char temp_name[UDOM_SIZE*2];
	
	mysql_adaptor_encode_squote(domainname, temp_name);
	auto qstr = "SELECT id, org_id FROM domains WHERE domainname='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*pdomain_id = strtoul(myrow[0], nullptr, 0);
	*porg_id    = strtoul(myrow[1], nullptr, 0);
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1720", e.what());
	return false;
}

BOOL mysql_adaptor_get_mlist_ids(unsigned int user_id, unsigned int *pgroup_id,
    unsigned int *pdomain_id) try
{
	auto qstr = "SELECT dt.propval_str AS dtypx, u.domain_id, u.group_id "
	            "FROM users AS u " JOIN_WITH_DISPLAYTYPE
	            " WHERE id=" + std::to_string(user_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	if (myrow == nullptr || myrow[0] == nullptr ||
	    static_cast<enum display_type>(strtoul(myrow[0], nullptr, 0)) != DT_DISTLIST)
		return FALSE;
	*pdomain_id = strtoul(myrow[1], nullptr, 0);
	*pgroup_id  = strtoul(myrow[2], nullptr, 0);
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1721", e.what());
	return false;
}

BOOL mysql_adaptor_get_org_domains(unsigned int org_id,
    std::vector<unsigned int> &pfile) try
{
	auto qstr = "SELECT id FROM domains WHERE org_id=" + std::to_string(org_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	size_t i, rows = pmyres.num_rows();
	pfile = std::vector<unsigned int>(rows);
	for (i=0; i<rows; i++) {
		auto myrow = pmyres.fetch_row();
		pfile[i] = strtoul(myrow[0], nullptr, 0);
	}
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1722", e.what());
	return false;
}

BOOL mysql_adaptor_get_domain_info(unsigned int domain_id, sql_domain &dinfo) try
{
	auto qstr = "SELECT domainname, title, address, homedir "
	            "FROM domains WHERE id=" + std::to_string(domain_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	if (myrow == nullptr)
		return false;
	dinfo.name = myrow[0];
	dinfo.title = myrow[1];
	dinfo.address = myrow[2];
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1723", e.what());
	return false;
}

BOOL mysql_adaptor_check_same_org(unsigned int domain_id1, unsigned int domain_id2) try
{
	auto qstr = "SELECT org_id FROM domains WHERE id=" + std::to_string(domain_id1) +
	            " OR id=" + std::to_string(domain_id2);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 2)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	auto org_id1 = strtoul(myrow[0], nullptr, 0);
	myrow = pmyres.fetch_row();
	auto org_id2 = strtoul(myrow[0], nullptr, 0);
	if (0 == org_id1 || 0 == org_id2 || org_id1 != org_id2) {
		return FALSE;
	}
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1724", e.what());
	return false;
}

BOOL mysql_adaptor_get_domain_groups(unsigned int domain_id,
    std::vector<sql_group> &pfile) try
{
	auto qstr = "SELECT `id`, `groupname`, `title` FROM `groups` "
	            "WHERE `domain_id`=" + std::to_string(domain_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	size_t i, rows = pmyres.num_rows();
	std::vector<sql_group> gv(rows);
	for (i=0; i<rows; i++) {
		auto myrow = pmyres.fetch_row();
		gv[i].id = strtoul(myrow[0], nullptr, 0);
		gv[i].name = myrow[1];
		gv[i].title = myrow[2];
	}
	pfile = std::move(gv);
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1725", e.what());
	return false;
}

BOOL mysql_adaptor_check_mlist_include(const char *mlist_name,
    const char *account) try
{
	BOOL b_result;
	char temp_name[UADDR_SIZE*2];
	char *pencode_domain;
	char temp_account[512];
	
	if (NULL == strchr(mlist_name, '@')) {
		return FALSE;
	}
	
	mysql_adaptor_encode_squote(mlist_name, temp_name);
	pencode_domain = strchr(temp_name, '@') + 1;
	mysql_adaptor_encode_squote(account, temp_account);
	auto qstr = "SELECT id, list_type FROM mlists WHERE listname='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	if (pmyres.num_rows() != 1)
		return FALSE;

	auto myrow = pmyres.fetch_row();
	unsigned int id = strtoul(myrow[0], nullptr, 0);
	auto type = static_cast<mlist_type>(strtoul(myrow[1], nullptr, 0));
	b_result = FALSE;
	switch (type) {
	case mlist_type::normal:
		qstr = "SELECT username FROM associations WHERE list_id=" +
		       std::to_string(id) + " AND username='"s + temp_account + "'";
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() > 0)
			b_result = TRUE;
		return b_result;
	case mlist_type::group: {
		qstr = "SELECT `id` FROM `groups` WHERE `groupname`='"s + temp_name + "'";
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1)
			return FALSE;
		myrow = pmyres.fetch_row();
		unsigned int group_id = strtoul(myrow[0], nullptr, 0);
		qstr = "SELECT username FROM users WHERE group_id=" + std::to_string(group_id) +
		       " AND username='" + temp_account + "'";
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() > 0)
			b_result = TRUE;
		return b_result;
	}
	case mlist_type::domain: {
		qstr = "SELECT id FROM domains WHERE domainname='"s + pencode_domain + "'";
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1)
			return FALSE;
		myrow = pmyres.fetch_row();
		unsigned int domain_id = strtoul(myrow[0], nullptr, 0);
		qstr = "SELECT username FROM users WHERE domain_id=" + std::to_string(domain_id) +
		       " AND username='" + temp_account + "'";
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() > 0)
			b_result = TRUE;
		return b_result;
	}
	case mlist_type::dyngroup: {
		return false;
	}
	default:
		return FALSE;
	}
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1729", e.what());
	return false;
}

void mysql_adaptor_encode_squote(const char *in, char *out)
{
	int len, i, j;

	len = strlen(in);
	for (i=0, j=0; i<len; i++, j++) {
		if ('\'' == in[i] || '\\' == in[i]) {
			out[j++] = '\\';
	}
		out[j] = in[i];
	}
	out[j] = '\0';
}

BOOL mysql_adaptor_check_same_org2(const char *domainname1,
    const char *domainname2) try
{
	char temp_name1[UDOM_SIZE*2], temp_name2[UDOM_SIZE*2];

	mysql_adaptor_encode_squote(domainname1, temp_name1);
	mysql_adaptor_encode_squote(domainname2, temp_name2);
	auto qstr = "SELECT org_id FROM domains WHERE domainname='"s + temp_name1 +
	            "' OR domainname='" + temp_name2 + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 2)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	auto org_id1 = strtoul(myrow[0], nullptr, 0);
	myrow = pmyres.fetch_row();
	auto org_id2 = strtoul(myrow[0], nullptr, 0);
	if (0 == org_id1 || 0 == org_id2 || org_id1 != org_id2) {
		return FALSE;
	}
	return TRUE;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1730", e.what());
	return false;
}

/* only used by delivery-queue; who can receive mail? */
bool mysql_adaptor_check_user(const char *username, char *path, size_t dsize) try
{
	char temp_name[UADDR_SIZE*2];

	if (path != nullptr)
		*path = '\0';
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT DISTINCT u.address_status, u.maildir FROM users AS u "
		"LEFT JOIN aliases AS a ON u.username=a.mainname "
		"WHERE u.username='"s + temp_name + "' OR a.aliasname='" +
		temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() == 0) {
		return false;
	} else if (pmyres.num_rows() > 1) {
		mlog(LV_WARN, "W-1510: userdb conflict: <%s> is in both \"users\" and \"aliases\"", username);
		return false;
	}
	auto myrow = pmyres.fetch_row();
	if (path != nullptr)
		gx_strlcpy(path, myrow[1], dsize);
	unsigned int status = strtoul(myrow[0], nullptr, 0);
	return status == AF_USER_NORMAL || status == AF_USER_SHAREDMBOX;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1731", e.what());
	return false;
}

/**
 * @from:	From address
 * @username:	Recipient address; mailing list
 * @pfile:	Output array - append, NO truncate
 */
BOOL mysql_adaptor_get_mlist_memb(const char *username,  const char *from,
    int *presult, std::vector<std::string> &pfile) try
{
	int i, rows;
	BOOL b_chkintl;
	char *pencode_domain;
	char temp_name[UADDR_SIZE*2];

	*presult = MLIST_RESULT_NONE;
	const char *pdomain = strchr(username, '@');
	if (NULL == pdomain) {
		return TRUE;
	}

	pdomain++;
	const char *pfrom_domain = strchr(from, '@');
	if (NULL == pfrom_domain) {
		return TRUE;
	}

	pfrom_domain++;
	mysql_adaptor_encode_squote(username, temp_name);
	pencode_domain = strchr(temp_name, '@') + 1;

	auto qstr = "SELECT id, list_type, list_privilege FROM mlists "
	            "WHERE listname='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	if (pmyres.num_rows() != 1) {
		*presult = MLIST_RESULT_NONE;
		return TRUE;
	}
	auto myrow = pmyres.fetch_row();
	unsigned int id = strtoul(myrow[0], nullptr, 0);
	auto type = static_cast<mlist_type>(strtoul(myrow[1], nullptr, 0));
	auto privilege = static_cast<mlist_priv>(strtoul(myrow[2], nullptr, 0));

	switch (privilege) {
	case mlist_priv::all:
	case mlist_priv::outgoing:
		b_chkintl = FALSE;
		break;
	case mlist_priv::internal:
		b_chkintl = TRUE;
		break;
	case mlist_priv::domain:
		if (0 != strcasecmp(pdomain, pfrom_domain)) {
			*presult = MLIST_RESULT_PRIVIL_DOMAIN;
			return TRUE;
		}
		b_chkintl = FALSE;
		break;
	case mlist_priv::specified:
		qstr = "SELECT username FROM specifieds WHERE list_id=" + std::to_string(id);
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		rows = pmyres.num_rows();
		for (i = 0; i < rows; i++) {
			myrow = pmyres.fetch_row();
			if (0 == strcasecmp(myrow[0], from) ||
				0 == strcasecmp(myrow[0], pfrom_domain)) {
				break;
			}
		}
		if (i == rows) {
			*presult = MLIST_RESULT_PRIVIL_SPECIFIED;
			return TRUE;
		}
		b_chkintl = FALSE;
		break;
	default:
		*presult = MLIST_RESULT_NONE;
		return TRUE;
	}

	switch (type) {
	case mlist_type::normal:
		qstr = "SELECT username FROM associations WHERE list_id=" + std::to_string(id);
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		rows = pmyres.num_rows();
		if (b_chkintl) {
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				if (0 == strcasecmp(myrow[0], from)) {
					b_chkintl = FALSE;
					break;
				}
			}
		}
		if (b_chkintl) {
			*presult = MLIST_RESULT_PRIVIL_INTERNAL;
			return TRUE;
		}
		mysql_data_seek(pmyres.get(), 0);
		for (i = 0; i < rows; i++) {
			myrow = pmyres.fetch_row();
			pfile.push_back(myrow[0]);
		}
		*presult = MLIST_RESULT_OK;
		return TRUE;
	case mlist_type::group: {
		qstr = "SELECT `id` FROM `groups` WHERE `groupname`='"s + temp_name + "'";
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1) {
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}
		myrow = pmyres.fetch_row();
		unsigned int group_id = strtoul(myrow[0], nullptr, 0);
		qstr = "SELECT u.username, dt.propval_str AS dtypx FROM users AS u "
		       JOIN_WITH_DISPLAYTYPE " WHERE u.group_id=" + std::to_string(group_id);
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		rows = pmyres.num_rows();
		if (b_chkintl) {
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				auto dtypx = DT_MAILUSER;
				if (myrow[1] != nullptr)
					dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
				if (dtypx == DT_MAILUSER && strcasecmp(myrow[0], from) == 0) {
					b_chkintl = FALSE;
					break;
				}
			}
		}
		if (b_chkintl) {
			*presult = MLIST_RESULT_PRIVIL_INTERNAL;
			return TRUE;
		}
		mysql_data_seek(pmyres.get(), 0);
		for (i = 0; i < rows; i++) {
			myrow = pmyres.fetch_row();
			auto dtypx = DT_MAILUSER;
			if (myrow[1] != nullptr)
				dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
			if (dtypx == DT_MAILUSER)
				pfile.push_back(myrow[0]);
		}
		*presult = MLIST_RESULT_OK;
		return TRUE;
	}
	case mlist_type::domain: {
		qstr = "SELECT id FROM domains WHERE domainname='"s + pencode_domain + "'";
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1) {
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}
		myrow = pmyres.fetch_row();
		unsigned int domain_id = strtoul(myrow[0], nullptr, 0);
		qstr = "SELECT u.username, dt.propval_str AS dtypx FROM users AS u "
		       JOIN_WITH_DISPLAYTYPE " WHERE u.domain_id=" + std::to_string(domain_id);
		if (!conn->query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn->get());
		if (pmyres == nullptr)
			return false;
		rows = pmyres.num_rows();
		if (b_chkintl) {
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				auto dtypx = DT_MAILUSER;
				if (myrow[1] != nullptr)
					dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
				if (dtypx == DT_MAILUSER && strcasecmp(myrow[0], from) == 0) {
					b_chkintl = FALSE;
					break;
				}
			}
		}
		if (b_chkintl) {
			*presult = MLIST_RESULT_PRIVIL_INTERNAL;
			return TRUE;
		}
		mysql_data_seek(pmyres.get(), 0);
		for (i = 0; i < rows; i++) {
			myrow = pmyres.fetch_row();
			auto dtypx = DT_MAILUSER;
			if (myrow[1] != nullptr)
				dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
			if (dtypx == DT_MAILUSER)
				pfile.push_back(myrow[0]);
		}
		*presult = MLIST_RESULT_OK;
		return TRUE;
	}
	case mlist_type::dyngroup: {
		*presult = MLIST_RESULT_OK;
		return TRUE;
	}
	default:
		*presult = MLIST_RESULT_NONE;
		return TRUE;
	}
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1732", e.what());
	return false;
}

/* only used by gromox-delivery/exmdb_local */
bool mysql_adaptor_get_user_info(const char *username, char *maildir,
    size_t msize, char *lang, size_t lsize, char *zone, size_t tsize) try
{
	char temp_name[UADDR_SIZE*2];

	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT maildir, address_status, lang, timezone "
	            "FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (*conn == nullptr)
		return false;
	if (!conn->query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn->get());
	if (pmyres == nullptr)
		return false;
	conn.finish();

	if (pmyres.num_rows() != 1) {
		maildir[0] = '\0';
		return true;
	}
	auto myrow = pmyres.fetch_row();
	auto status = strtoul(myrow[1], nullptr, 0);
	if (status == AF_USER_NORMAL || status == AF_USER_SHAREDMBOX) {
		gx_strlcpy(maildir, myrow[0], msize);
		gx_strlcpy(lang, myrow[2], lsize);
		gx_strlcpy(zone, myrow[3], tsize);
	} else {
		maildir[0] = '\0';
		lang[0] = '\0';
		zone[0] = '\0';
	}
	return true;
} catch (const std::exception &e) {
	mlog(LV_ERR, "%s: %s", "E-1733", e.what());
	return false;
}
