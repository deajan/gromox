// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <typeinfo>
#include <unistd.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/util.hpp>
#include <gromox/guid.hpp>
#include <gromox/oxcmail.hpp>
#include <gromox/str_hash.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/list_file.hpp>
#include "cache_queue.h"
#include "exmdb_local.h"
#include "net_failure.h"
#include "exmdb_client.h"
#include "bounce_audit.h"
#include "auto_response.h"
#include <gromox/alloc_context.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>


#define MAX_DIGLEN				256*1024

#define DEF_MODE				S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH

using namespace gromox;

static char g_org_name[256];
static pthread_key_t g_alloc_key;
static std::unique_ptr<STR_HASH_TABLE> g_str_hash;
static char g_default_charset[32];
static char g_default_timezone[64];
static std::atomic<int> g_sequence_id;

int (*exmdb_local_check_domain)(const char *domainname);

static bool (*exmdb_local_get_user_info)(const char *username, char *home_dir, size_t dsize, char *lang, size_t lsize, char *timezone, size_t tsize);
bool (*exmdb_local_get_lang)(const char *username, char *lang, size_t);
bool (*exmdb_local_get_timezone)(const char *username, char *timezone, size_t);
BOOL (*exmdb_local_check_same_org2)(
	const char *domainname1, const char *domainname2);

BOOL (*exmdb_local_lang_to_charset)(const char *lang, char *charset);

static uint32_t (*exmdb_local_ltag_to_lcid)(const char*);

static const char* (*exmdb_local_lcid_to_ltag)(uint32_t);

static uint32_t (*exmdb_local_charset_to_cpid)(const char*);

static const char* (*exmdb_local_cpid_to_charset)(uint32_t);

static const char* (*exmdb_local_mime_to_extension)(const char*);

static const char* (*exmdb_local_extension_to_mime)(const char*);
static BOOL (*exmdb_local_get_user_ids)(const char *, int *, int *, enum display_type *);
static BOOL (*exmdb_local_get_username)(int, char *, size_t);

static int exmdb_local_sequence_ID()
{
	int old = 0, nu = 0;
	do {
		old = g_sequence_id.load(std::memory_order_relaxed);
		nu  = old != INT_MAX ? old + 1 : 1;
	} while (!g_sequence_id.compare_exchange_weak(old, nu));
	return nu;
}


void exmdb_local_init(const char *org_name, const char *default_charset,
    const char *default_timezone)
{
	gx_strlcpy(g_org_name, org_name, GX_ARRAY_SIZE(g_org_name));
	gx_strlcpy(g_default_charset, default_charset, GX_ARRAY_SIZE(g_default_charset));
	gx_strlcpy(g_default_timezone, default_timezone, GX_ARRAY_SIZE(g_default_timezone));
	pthread_key_create(&g_alloc_key, NULL);
}

int exmdb_local_run()
{
	int last_propid;
	char temp_line[256];
	
#define E(f, s) do { \
	query_service2((s), f); \
	if ((f) == nullptr) { \
		printf("[%s]: failed to get the \"%s\" service\n", "exmdb_local", (s)); \
		return -1; \
	} \
} while (false)

	E(exmdb_local_check_domain, "domain_list_query");
	E(exmdb_local_get_user_info, "get_user_info");
	E(exmdb_local_get_lang, "get_user_lang");
	E(exmdb_local_get_timezone, "get_timezone");
	E(exmdb_local_check_same_org2, "check_same_org2");
	E(exmdb_local_lang_to_charset, "lang_to_charset");
	E(exmdb_local_ltag_to_lcid, "ltag_to_lcid");
	E(exmdb_local_lcid_to_ltag, "lcid_to_ltag");
	E(exmdb_local_charset_to_cpid, "charset_to_cpid");
	E(exmdb_local_cpid_to_charset, "cpid_to_charset");
	E(exmdb_local_mime_to_extension, "mime_to_extension");
	E(exmdb_local_extension_to_mime, "extension_to_mime");
	E(exmdb_local_get_user_ids, "get_user_ids");
	E(exmdb_local_get_username, "get_username_from_id");
#undef E

	if (FALSE == oxcmail_init_library(g_org_name,
		exmdb_local_get_user_ids, exmdb_local_get_username,
		exmdb_local_ltag_to_lcid, exmdb_local_lcid_to_ltag,
		exmdb_local_charset_to_cpid, exmdb_local_cpid_to_charset,
		exmdb_local_mime_to_extension, exmdb_local_extension_to_mime)) {
		printf("[exmdb_local]: Failed to init oxcmail library\n");
		return -2;
	}
	struct srcitem { char s[256]; };
	auto plist = list_file_initd("propnames.txt", get_data_path(), "%s:256");
	if (NULL == plist) {
		printf("[exmdb_local]: list_file_initd propnames.txt: %s\n", strerror(errno));
		return -3;
	}
	auto num = plist->get_size();
	auto pitem = static_cast<srcitem *>(plist->get_list());
	g_str_hash = STR_HASH_TABLE::create(num + 1, sizeof(uint16_t), nullptr);
	if (NULL == g_str_hash) {
		printf("[exmdb_local]: Failed to init hash table\n");
		return -4;
	}
	last_propid = 0x8001;
	for (decltype(num) i = 0; i < num; ++i) {
		gx_strlcpy(temp_line, pitem[i].s, sizeof(temp_line));
		HX_strlower(temp_line);
		g_str_hash->add(temp_line, &last_propid);
		last_propid ++;
	}
	return 0;
}

void exmdb_local_free()
{
	pthread_key_delete(g_alloc_key);
}

BOOL exmdb_local_hook(MESSAGE_CONTEXT *pcontext)
{
	int cache_ID;
	char *pdomain;
	BOOL remote_found;
	char rcpt_buff[256];
	time_t current_time;
	MEM_FILE remote_file;
	MESSAGE_CONTEXT *pbounce_context;
	
	remote_found = FALSE;
	if (BOUND_NOTLOCAL == pcontext->pcontrol->bound_type) {
		return FALSE;
	}
	mem_file_init(&remote_file, pcontext->pcontrol->f_rcpt_to.allocator);
	while (pcontext->pcontrol->f_rcpt_to.readline(rcpt_buff,
	       arsizeof(rcpt_buff)) != MEM_END_OF_FILE) {
		pdomain = strchr(rcpt_buff, '@');
		if (NULL == pdomain) {
			remote_file.writeline(rcpt_buff);
			continue;
		}
		pdomain ++;
		auto lcldom = exmdb_local_check_domain(pdomain);
		if (lcldom < 0)
			continue;
		if (lcldom == 0) {
			remote_found = TRUE;
			remote_file.writeline(rcpt_buff);
			continue;
		}
		switch (exmdb_local_deliverquota(pcontext, rcpt_buff)) {
		case DELIVERY_OPERATION_OK:
			net_failure_statistic(1, 0, 0, 0);
			break;
		case DELIVERY_OPERATION_DELIVERED:
			net_failure_statistic(1, 0, 0, 0);
			if (!pcontext->pcontrol->need_bounce ||
			    strcasecmp(pcontext->pcontrol->from, "none@none") == 0)
				break;
			pbounce_context = get_context();
			if (NULL == pbounce_context) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
					"fail to get bounce context");
				break;
			}
			if (FALSE == bounce_audit_check(rcpt_buff)) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
					"will not produce bounce message, "
					"because of too many mails to %s", rcpt_buff);
				put_context(pbounce_context);
				break;
			}
			time(&current_time);
			bounce_producer_make(pcontext->pcontrol->from,
				rcpt_buff, pcontext->pmail, current_time,
				BOUNCE_MAIL_DELIVERED, pbounce_context->pmail);
			pbounce_context->pcontrol->need_bounce = FALSE;
			sprintf(pbounce_context->pcontrol->from,
				"postmaster@%s", get_default_domain());
			pbounce_context->pcontrol->f_rcpt_to.writeline(pcontext->pcontrol->from);
			enqueue_context(pbounce_context);
			break;
		case DELIVERY_NO_USER:
			net_failure_statistic(0, 0, 0, 1);
			if (!pcontext->pcontrol->need_bounce ||
			    strcasecmp(pcontext->pcontrol->from, "none@none") == 0)
				break;
			pbounce_context = get_context();
			if (NULL == pbounce_context) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
					"fail to get bounce context");
				break;
			}
			if (FALSE == bounce_audit_check(rcpt_buff)) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
					"will not produce bounce message, "
					"because of too many mails to %s", rcpt_buff);
				put_context(pbounce_context);
				break;
			}
			time(&current_time);
			bounce_producer_make(pcontext->pcontrol->from,
				rcpt_buff, pcontext->pmail, current_time,
				BOUNCE_NO_USER, pbounce_context->pmail);
			pbounce_context->pcontrol->need_bounce = FALSE;
			sprintf(pbounce_context->pcontrol->from,
				"postmaster@%s", get_default_domain());
			pbounce_context->pcontrol->f_rcpt_to.writeline(pcontext->pcontrol->from);
			enqueue_context(pbounce_context);
			break;
		case DELIVERY_MAILBOX_FULL:
			if (!pcontext->pcontrol->need_bounce ||
			    strcasecmp(pcontext->pcontrol->from, "none@none") == 0)
				break;
			pbounce_context = get_context();
			if (NULL == pbounce_context) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
					"fail to get bounce context");
				break;
			}
			if (FALSE == bounce_audit_check(rcpt_buff)) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
					"will not produce bounce message, "
					"because of too many mails to %s", rcpt_buff);
				put_context(pbounce_context);
				break;
			}
			time(&current_time);
			bounce_producer_make(pcontext->pcontrol->from,
				rcpt_buff, pcontext->pmail, current_time,
				BOUNCE_MAILBOX_FULL, pbounce_context->pmail);
			pbounce_context->pcontrol->need_bounce = FALSE;
			sprintf(pbounce_context->pcontrol->from,
				"postmaster@%s", get_default_domain());
			pbounce_context->pcontrol->f_rcpt_to.writeline(pcontext->pcontrol->from);
			enqueue_context(pbounce_context);
			break;
		case DELIVERY_OPERATION_ERROR:
			net_failure_statistic(0, 0, 1, 0);
			if (!pcontext->pcontrol->need_bounce ||
			    strcasecmp(pcontext->pcontrol->from, "none@none") == 0)
				break;
			pbounce_context = get_context();
			if (NULL == pbounce_context) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
					"fail to get bounce context");
				break;
			}
			if (FALSE == bounce_audit_check(rcpt_buff)) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
					"will not produce bounce message, "
					"because of too many mails to %s", rcpt_buff);
				put_context(pbounce_context);
				break;
			}
			time(&current_time);
			bounce_producer_make(pcontext->pcontrol->from,
				rcpt_buff, pcontext->pmail, current_time,
				BOUNCE_OPERATION_ERROR, pbounce_context->pmail);
			pbounce_context->pcontrol->need_bounce = FALSE;
			sprintf(pbounce_context->pcontrol->from,
				"postmaster@%s", get_default_domain());
			pbounce_context->pcontrol->f_rcpt_to.writeline(pcontext->pcontrol->from);
			enqueue_context(pbounce_context);
			break;
		case DELIVERY_OPERATION_FAILURE:
			net_failure_statistic(0, 1, 0, 0);
			time(&current_time);
			cache_ID = cache_queue_put(pcontext, rcpt_buff, current_time);
			if (cache_ID >= 0) {
				exmdb_local_log_info(pcontext, rcpt_buff, LV_INFO,
					"message is put into cache queue with cache ID %d and "
					"wait to be delivered next time", cache_ID);
				break;
			}
			exmdb_local_log_info(pcontext, rcpt_buff, LV_ERR,
				"failed to put message into cache queue");
			break;
		}
	}
	if (TRUE == remote_found) {
		remote_file.copy_to(pcontext->pcontrol->f_rcpt_to);
		mem_file_free(&remote_file);
		return FALSE;
	}
	mem_file_free(&remote_file);
	return TRUE;
}

static void* exmdb_local_alloc(size_t size)
{
	auto pctx = static_cast<ALLOC_CONTEXT *>(pthread_getspecific(g_alloc_key));
	if (NULL == pctx) {
		return NULL;
	}
	return alloc_context_alloc(pctx, size);
}

static BOOL exmdb_local_get_propids(const PROPNAME_ARRAY *ppropnames,
    PROPID_ARRAY *ppropids)
{
	int i;
	
	ppropids->count = ppropnames->count;
	ppropids->ppropid = static_cast<uint16_t *>(exmdb_local_alloc(sizeof(uint16_t) * ppropnames->count));
	for (i=0; i<ppropnames->count; i++) {
		char tmp_string[NP_STRBUF_SIZE], tmp_guid[GUIDSTR_SIZE];
		guid_to_string(&ppropnames->ppropname[i].guid, tmp_guid, arsizeof(tmp_guid));
		if (ppropnames->ppropname[i].kind == MNID_ID)
			snprintf(tmp_string, arsizeof(tmp_string), "GUID=%s,LID=%u",
			         tmp_guid, ppropnames->ppropname[i].lid);
		else
			snprintf(tmp_string, arsizeof(tmp_string), "GUID=%s,NAME=%s",
				tmp_guid, ppropnames->ppropname[i].pname);

		HX_strlower(tmp_string);
		auto ppropid = g_str_hash->query<uint16_t>(tmp_string);
		if (NULL == ppropid) {
			ppropids->ppropid[i] = 0;
		} else {
			ppropids->ppropid[i] = *ppropid;
		}
	}
	return TRUE;
}


int exmdb_local_deliverquota(MESSAGE_CONTEXT *pcontext, const char *address)
{
	MAIL *pmail;
	int tmp_len;
	void *pvalue;
	size_t mess_len;
	int sequence_ID;
	time_t cur_time;
	uint64_t nt_time;
	char lang[32], charset[32], tmzone[64], hostname[UDOM_SIZE], home_dir[256];
	uint32_t tmp_int32;
	uint32_t suppress_mask;
	BOOL b_bounce_delivered = false;
	ALLOC_CONTEXT alloc_ctx;
	char temp_buff[MAX_DIGLEN];
	MESSAGE_CONTEXT *pcontext1;

	if (!exmdb_local_get_user_info(address, home_dir, arsizeof(home_dir),
	    lang, arsizeof(lang), tmzone, arsizeof(tmzone))) {
		exmdb_local_log_info(pcontext, address, LV_ERR, "fail"
			"to get user information from data source!");
		return DELIVERY_OPERATION_FAILURE;
	}
	if ('\0' == lang[0] || FALSE ==
		exmdb_local_lang_to_charset(lang,
		charset) || '\0' == charset[0]) {
		strcpy(charset, g_default_charset);
	}
	if ('\0' == home_dir[0]) {
		exmdb_local_log_info(pcontext, address, LV_ERR,
			"<%s> has no mailbox here", address);
		return DELIVERY_NO_USER;
	}
	if (tmzone[0] == '\0')
		strcpy(tmzone, g_default_timezone);
	
	pmail = pcontext->pmail;
	if (pcontext->pmail->check_dot()) {
		pcontext1 = get_context();
		if (NULL != pcontext1) {
			if (pcontext->pmail->transfer_dot(pcontext1->pmail)) {
				pmail = pcontext1->pmail;
			} else {
				put_context(pcontext1);
				pcontext1 = NULL;
			}
		}
	} else {
		pcontext1 = NULL;
	}
	
	time(&cur_time);
	sequence_ID = exmdb_local_sequence_ID();
	gx_strlcpy(hostname, get_host_ID(), arsizeof(hostname));
	if ('\0' == hostname[0]) {
		if (gethostname(hostname, arsizeof(hostname)) < 0)
			strcpy(hostname, "localhost");
		else
			hostname[arsizeof(hostname)-1] = '\0';
	}
	std::string mid_string, json_string, eml_path;
	int fd = -1;
	try {
		mid_string = std::to_string(cur_time) + "." +
		             std::to_string(sequence_ID) + "." + hostname;
		eml_path = std::string(home_dir) + "/eml/" + mid_string;
		fd = open(eml_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, DEF_MODE);
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1472: ENOMEM\n");
	}
	if (-1 == fd) {
		if (NULL != pcontext1) {
			put_context(pcontext1);
		}
		exmdb_local_log_info(pcontext, address, LV_ERR,
			"open WR %s: %s", eml_path.c_str(), strerror(errno));
		return DELIVERY_OPERATION_FAILURE;
	}
	
	if (!pmail->to_file(fd)) {
		close(fd);
		if (remove(eml_path.c_str()) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1386: remove %s: %s\n",
			        eml_path.c_str(), strerror(errno));
		if (NULL != pcontext1) {
			put_context(pcontext1);
		}
		exmdb_local_log_info(pcontext, address, LV_ERR,
			"%s: pmail->to_file failed for unspecified reasons", eml_path.c_str());
		return DELIVERY_OPERATION_FAILURE;
	}
	close(fd);

	tmp_len = sprintf(temp_buff, "{\"file\":\"%s\",", mid_string.c_str());
	int result = pmail->get_digest(&mess_len, temp_buff + tmp_len,
				MAX_DIGLEN - tmp_len - 1);
	
	if (result <= 0) {
		if (remove(eml_path.c_str()) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1387: remove %s: %s\n",
			        eml_path.c_str(), strerror(errno));
		if (NULL != pcontext1) {
			put_context(pcontext1);
		}
		exmdb_local_log_info(pcontext, address, LV_ERR,
			"permanent failure getting mail digest");
		return DELIVERY_OPERATION_ERROR;
	}
	tmp_len = strlen(temp_buff);
	temp_buff[tmp_len] = '}';
	tmp_len ++;
	temp_buff[tmp_len] = '\0';
	
	alloc_context_init(&alloc_ctx);
	pthread_setspecific(g_alloc_key, &alloc_ctx);
	auto pmsg = oxcmail_import(charset, tmzone, pmail, exmdb_local_alloc,
	            exmdb_local_get_propids);
	if (NULL != pcontext1) {
		put_context(pcontext1);
	}
	if (NULL == pmsg) {
		alloc_context_free(&alloc_ctx);
		pthread_setspecific(g_alloc_key, NULL);
		if (remove(eml_path.c_str()) < 0 && errno != ENOENT)
			fprintf(stderr, "W-1388: remove %s: %s\n",
			        eml_path.c_str(), strerror(errno));
		exmdb_local_log_info(pcontext, address, LV_ERR, "fail "
			"to convert rfc5322 into MAPI message object");
		return DELIVERY_OPERATION_ERROR;
	}
	alloc_context_free(&alloc_ctx);
	pthread_setspecific(g_alloc_key, NULL);
	
	nt_time = rop_util_current_nttime();
	pmsg->proplist.set(PROP_TAG_MESSAGEDELIVERYTIME, &nt_time);
	
	if (FALSE == pcontext->pcontrol->need_bounce) {
		tmp_int32 = 0xFFFFFFFF;
		pmsg->proplist.set(PR_AUTO_RESPONSE_SUPPRESS, &tmp_int32);
	}
	
	pmsg->proplist.erase(PidTagChangeNumber);
	result = exmdb_client_delivery_message(
		home_dir, pcontext->pcontrol->from,
		address, 0, pmsg, temp_buff);
	if (EXMDB_RESULT_OK == result) {
		pvalue = pmsg->proplist.getval(PR_AUTO_RESPONSE_SUPPRESS);
		if (NULL == pvalue) {
			suppress_mask = 0;
		} else {
			suppress_mask = *(uint32_t*)pvalue;
		}
		pvalue = pmsg->proplist.getval(PR_ORIGINATOR_DELIVERY_REPORT_REQUESTED);
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			b_bounce_delivered = TRUE;
			if (suppress_mask & AUTO_RESPONSE_SUPPRESS_DR) {
				b_bounce_delivered = FALSE;
			}
		} else {
			b_bounce_delivered = FALSE;
		}
	}
	message_content_free(pmsg);
	switch (result) {
	case EXMDB_RESULT_OK:
		exmdb_local_log_info(pcontext, address, LV_DEBUG,
			"message %s was delivered OK", eml_path.c_str());
		if (TRUE == pcontext->pcontrol->need_bounce &&
			0 != strcmp(pcontext->pcontrol->from, "none@none") &&
			0 == (suppress_mask & AUTO_RESPONSE_SUPPRESS_OOF)) {
			auto_response_reply(home_dir, address, pcontext->pcontrol->from);
		}
		if (TRUE == b_bounce_delivered) {
			return DELIVERY_OPERATION_DELIVERED;
		}
		return DELIVERY_OPERATION_OK;
	case EXMDB_RUNTIME_ERROR:
		exmdb_local_log_info(pcontext, address, LV_ERR,
			"rpc run-time error when delivering "
			"message into directory %s!", home_dir);
		return DELIVERY_OPERATION_FAILURE;
	case EXMDB_NO_SERVER:
		exmdb_local_log_info(pcontext, address, LV_ERR,
			"missing exmdb server connection when "
			"delivering message into directory %s!",
			home_dir);
		return DELIVERY_OPERATION_FAILURE;
	case EXMDB_RDWR_ERROR:
		exmdb_local_log_info(pcontext, address, LV_ERR,
			"read write error with exmdb server when"
			" delivering message into directory %s!",
			home_dir);
		return DELIVERY_OPERATION_FAILURE;
	case EXMDB_RESULT_ERROR:
		exmdb_local_log_info(pcontext, address, LV_ERR,
			"error result returned when delivering "
			"message into directory %s!", home_dir);
		return DELIVERY_OPERATION_FAILURE;
	case EXMDB_MAILBOX_FULL:
		exmdb_local_log_info(pcontext, address,
			LV_NOTICE, "user's mailbox is full");
		return DELIVERY_MAILBOX_FULL;
	}
	return DELIVERY_OPERATION_FAILURE;
}

void exmdb_local_log_info(MESSAGE_CONTEXT *pcontext,
    const char *rcpt_to, int level, const char *format, ...)
{
	char log_buf[256];
	va_list ap;

	va_start(ap, format);
	vsnprintf(log_buf, sizeof(log_buf) - 1, format, ap);
	va_end(ap);
	log_buf[sizeof(log_buf) - 1] = '\0';

	switch (pcontext->pcontrol->bound_type) {
	case BOUND_IN:
	case BOUND_OUT:
	case BOUND_RELAY:
		log_info(level, "SMTP message queue-ID: %d, FROM: %s, TO: %s  %s",
			pcontext->pcontrol->queue_ID, pcontext->pcontrol->from, rcpt_to,
			log_buf);
		break;
	default:
		log_info(level, "APP created message FROM: %s, TO: %s  %s",
			pcontext->pcontrol->from, rcpt_to, log_buf);
		break;
	}
}
