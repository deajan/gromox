#pragma once
#include <memory>
#include <vector>
#include <gromox/defs.h>
#include <gromox/element_data.hpp>
#include <gromox/ext_buffer.hpp>
#include <gromox/mail.hpp>

enum {
	VCARD_MAX_BUFFER_LEN = 1048576U,
};

enum class oxcmail_body {
	plain_only = 1,
	html_only = 2,
	plain_and_html = 3,
};

struct ical;
struct message_content;
using MESSAGE_CONTENT = message_content;
struct MIME_POOL;
struct vcard;

namespace gromox {

struct addr_tags {
	uint32_t pr_name, pr_addrtype, pr_emaddr, pr_smtpaddr, pr_entryid;
};

extern GX_EXPORT bool g_oxcical_allday_ymd, oxcical_exchsched_compat;
extern GX_EXPORT unsigned int g_oxvcard_pedantic;

}

extern GX_EXPORT ec_error_t oxcical_import_multi(const char *zone, const ical &, EXT_BUFFER_ALLOC, GET_PROPIDS, USERNAME_TO_ENTRYID, std::vector<std::unique_ptr<MESSAGE_CONTENT, gromox::mc_delete>> &);
extern GX_EXPORT std::unique_ptr<MESSAGE_CONTENT, gromox::mc_delete> oxcical_import_single(const char *zone, const ical &, EXT_BUFFER_ALLOC, GET_PROPIDS, USERNAME_TO_ENTRYID);
extern GX_EXPORT BOOL oxcical_export(const MESSAGE_CONTENT *, ical &, EXT_BUFFER_ALLOC, GET_PROPIDS, ENTRYID_TO_USERNAME, ESSDN_TO_USERNAME);

extern BOOL oxcmail_init_library(const char *org_name, GET_USER_IDS, GET_USERNAME);
extern MESSAGE_CONTENT *oxcmail_import(const char *charset, const char *timezone, const MAIL *, EXT_BUFFER_ALLOC, GET_PROPIDS);
extern GX_EXPORT BOOL oxcmail_export(const MESSAGE_CONTENT *, BOOL tnef, enum oxcmail_body, std::shared_ptr<MIME_POOL>, MAIL *, EXT_BUFFER_ALLOC, GET_PROPIDS, GET_PROPNAME);
extern GX_EXPORT BOOL oxcmail_username_to_entryid(const char *user, const char *disp, BINARY *, enum display_type *);
extern GX_EXPORT enum oxcmail_body get_override_format(const MESSAGE_CONTENT &);
extern GX_EXPORT BOOL oxcmail_entryid_to_username(const BINARY *, EXT_BUFFER_ALLOC, char *, size_t);
extern GX_EXPORT BOOL oxcmail_essdn_to_username(const char *, char *, size_t);
extern GX_EXPORT BOOL oxcmail_get_smtp_address(const TPROPVAL_ARRAY &, const gromox::addr_tags *, ENTRYID_TO_USERNAME, ESSDN_TO_USERNAME, EXT_BUFFER_ALLOC, char *outbuf, size_t outlen);

extern GX_EXPORT MESSAGE_CONTENT *oxvcard_import(const vcard *, GET_PROPIDS);
extern GX_EXPORT BOOL oxvcard_export(MESSAGE_CONTENT *, vcard &, GET_PROPIDS);
