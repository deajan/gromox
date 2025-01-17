// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2022-2023 grommunio GmbH
// This file is part of Gromox.

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>

#include <tinyxml2.h>
#include <fmt/core.h>
#include <gromox/config_file.hpp>
#include <gromox/exmdb_client.hpp>
#include <gromox/hpm_common.h>
#include <gromox/mime_pool.hpp>
#include <gromox/paths.h>
#include <gromox/rop_util.hpp>
#include <gromox/scope.hpp>

#include "exceptions.hpp"
#include "hash.hpp"
#include "requests.hpp"
#include "soaputil.hpp"

DECLARE_HPM_API();

namespace gromox::EWS::Exceptions
{

/**
 * @brief      Initialize EWSError
 *
 * @param t    EWS ResponseCode
 * @param m    Error message
 */
EWSError::EWSError(const char* t, const std::string& m) : DispatchError(m), type(t)
{}

void DispatchError::unused() {}
void EWSError::unused() {}

} // gromox::EWS::Exceptions

using namespace gromox;
using namespace gromox::EWS;
using namespace tinyxml2;

using Exceptions::DispatchError;

/**
 * @brief      Context data for debugging purposes
 *
 * Will only be present if explicitely requested by the
 * `ews_debug` configuration directive.
 */
struct EWSPlugin::DebugCtx
{
	static constexpr uint8_t FL_LOCK = 1 << 0;
	static constexpr uint8_t FL_RATELIMIT = 1 << 1;

	explicit DebugCtx(const std::string_view&);

	std::mutex requestLock{};
	std::chrono::high_resolution_clock::time_point last;
	std::chrono::high_resolution_clock::duration minRequestTime{};
	uint8_t flags = 0;
};

/**
 * @brief      Initialize debugging context
 *
 * Takes a string containing comma-separated debugging options.
 * Supported options are:
 * - `sequential`: Disable parallel processing of requests
 * - `rate_limit=<x>`: Only process <x> requests per second.
 *   Currently implies `sequential` (might be changed in the future).
 *
 * @param      opts    Debugging option string
 */
EWSPlugin::DebugCtx::DebugCtx(const std::string_view& opts)
{
	size_t start = 0;
	for(size_t end = opts.find(',', start); start != std::string_view::npos; end = opts.find(',', start))
	{
		std::string_view opt = opts.substr(start, end-start);
		start = end+(end != std::string_view::npos);
		if(opt == "sequential")
			flags |= FL_LOCK;
		else if(opt.substr(0, 11) == "rate_limit=")
		{
			unsigned long rateLimit = uint32_t(std::stoul(std::string(opt.substr(11))));
			if(rateLimit)
			{
				flags |= FL_RATELIMIT | FL_LOCK;
				minRequestTime = std::chrono::nanoseconds(1000000000/rateLimit);
			}
		}
		else
			mlog(LV_WARN, "[ews] Ignoring unknown debug directive '%s'", std::string(opt).c_str());
	}
}

///////////////////////////////////////////////////////////////////////////////

bool EWSPlugin::_exmdb::get_message_property(const char *dir, const char *username, cpid_t cpid, uint64_t message_id,
                                             uint32_t proptag, void **ppval) const
{
	PROPTAG_ARRAY tmp_proptags{1, &proptag};
	TPROPVAL_ARRAY propvals;

	if (!get_message_properties(dir, username, cpid, message_id, &tmp_proptags, &propvals))
		return false;
	*ppval = propvals.count == 1 && propvals.ppropval->proptag == proptag? propvals.ppropval->pvalue : nullptr;
	return true;
}


void* EWSContext::alloc(size_t count)
{return ndr_stack_alloc(NDR_STACK_IN, count);}

///////////////////////////////////////////////////////////////////////////////

/**
 * @brief      Deserialize request data and call processing function
 *
 * Provides a convenient handler function template when complete
 * de-serialization of request data is desired.
 *
 * @param      request   Request data
 * @param      response  Response data
 *
 * @tparam     T         Request data type
 */
template<typename T>
static void process(const XMLElement* request, XMLElement* response, const EWSContext& context)
{Requests::process(T(request), response, context);}

/**
 * Mapping of request names to handler functions.
 */
const std::unordered_map<std::string, EWSPlugin::Handler> EWSPlugin::requestMap =
{
	{"CopyFolder", process<Structures::mCopyFolderRequest>},
	{"CopyItem", process<Structures::mCopyItemRequest>},
	{"CreateFolder", process<Structures::mCreateFolderRequest>},
	{"CreateItem", process<Structures::mCreateItemRequest>},
	{"DeleteFolder", process<Structures::mDeleteFolderRequest>},
	{"DeleteItem", process<Structures::mDeleteItemRequest>},
	{"EmptyFolder", process<Structures::mEmptyFolderRequest>},
	{"GetAttachment", process<Structures::mGetAttachmentRequest>},
	{"GetFolder", process<Structures::mGetFolderRequest>},
	{"GetItem", process<Structures::mGetItemRequest>},
	{"GetMailTips", process<Structures::mGetMailTipsRequest>},
	{"GetServiceConfiguration", process<Structures::mGetServiceConfigurationRequest>},
	{"GetUserAvailabilityRequest", process<Structures::mGetUserAvailabilityRequest>},
	{"GetUserOofSettingsRequest", process<Structures::mGetUserOofSettingsRequest>},
	{"MoveFolder", process<Structures::mMoveFolderRequest>},
	{"MoveItem", process<Structures::mMoveItemRequest>},
	{"ResolveNames", process<Structures::mResolveNamesRequest>},
	{"SendItem", process<Structures::mSendItemRequest>},
	{"SetUserOofSettingsRequest", process<Structures::mSetUserOofSettingsRequest>},
	{"SyncFolderHierarchy", process<Structures::mSyncFolderHierarchyRequest>},
	{"SyncFolderItems", process<Structures::mSyncFolderItemsRequest>},
	{"UpdateFolder", process<Structures::mUpdateFolderRequest>},
	{"UpdateItem", process<Structures::mUpdateItemRequest>},
};

///////////////////////////////////////////////////////////////////////////////

/**
 * @brief      Preprocess request
 *
 * @param      ctx_id  Request context identifier
 *
 * @return     TRUE if the request is to be processed by this plugin, false otherwise
 */
BOOL EWSPlugin::preproc(int ctx_id)
{
	auto req = get_request(ctx_id);
	return strcasecmp(req->f_request_uri.c_str(), "/EWS/Exchange.asmx") == 0 ? TRUE : false;
}

/**
 * @brief      Write basic response header
 *
 * @param      ctx_id          Request context identifier
 * @param      code            HTTP response code
 * @param      content_length  Length of the response body
 */
void EWSPlugin::writeheader(int ctx_id, int code, size_t content_length)
{
	static constexpr char templ[] =
	        "HTTP/1.1 {} {}\r\n"
	        "Content-Type: text/xml\r\n"
	        "Content-Length: {}\r\n"
	        "\r\n";
	const char* status = "OK";
	switch(code) {
	case 400: status = "Bad Request"; break;
	case 500: status = "Internal Server Error"; break;
	}
	auto rs = fmt::format(templ, code, status, content_length);
	write_response(ctx_id, rs.c_str(), rs.size());
}

/**
 * @brief      Return authentication error
 *
 * @param      ctx_id  Request context identifier
 *
 * @return     TRUE if response was written successfully, false otherwise
 */
static BOOL unauthed(int ctx_id)
{
	static constexpr char content[] =
	        "HTTP/1.1 401 Unauthorized\r\n"
	        "Content-Length: 0\r\n"
	        "Connection: Keep-Alive\r\n"
	        "WWW-Authenticate: Basic realm=\"ews realm\"\r\n"
	        "\r\n";
	return write_response(ctx_id, content, strlen(content));
}

/**
 * @brief      Proccess request
 *
 * Checks if an authentication context exists, dispatches the request and
 * writes the response.
 *
 * @param      ctx_id   Request context identifier
 * @param      content  Request data
 * @param      len      Length of request data
 *
 * @return     TRUE if request was handled, false otherwise
 */
BOOL EWSPlugin::proc(int ctx_id, const void* content, uint64_t len)
{
	auto start = std::chrono::high_resolution_clock::now();
	auto req = get_request(ctx_id);
	if (strcasecmp(req->method, "POST") != 0) {
		static constexpr char rsp[] =
			"HTTP/1.1 405 Method Not Allowed\r\n"
			"Content-Length: 0\r\n"
			"Connection: Keep-Alive\r\n"
			"WWW-Authenticate: Basic realm=\"ews realm\"\r\n"
			"\r\n";
		return write_response(ctx_id, rsp, strlen(rsp));
	}
	HTTP_AUTH_INFO auth_info = get_auth_info(ctx_id);
	if(!auth_info.b_authed)
		return unauthed(ctx_id);
	bool enableLog = false;
	auto[response, code] = dispatch(ctx_id, auth_info, content, len, enableLog);
	auto logLevel = code == 200? LV_DEBUG : LV_ERR;
	if(enableLog && response_logging >= 2)
		mlog(logLevel, "[ews#%d] Response: %s", ctx_id, response.c_str());
	if(enableLog && response_logging)
	{
		auto end = std::chrono::high_resolution_clock::now();
		double duration = double(std::chrono::duration_cast<std::chrono::microseconds>(end-start).count()) / 1000.0;
		mlog(logLevel, "[ews#%d] Done, code %d, %zu bytes, %.3fms", ctx_id, code, response.size(), duration);
	}
	if(response.length() > std::numeric_limits<int>::max())
	{
		response = SOAP::Envelope::fault("Server", "Response body to large");
		code = 500;
	}
	writeheader(ctx_id, code, response.length());
	return write_response(ctx_id, response.c_str(), int(response.length()));
}

/**
 * @brief      Dispatch request to appropriate handler
 *
 * @param      ctx_id     Request context identifier
 * @param      auth_info  Authentication context
 * @param      data       Request data
 * @param      len        Length of request data
 *
 * @return     Pair of response content and HTTP response code
 */
std::pair<std::string, int> EWSPlugin::dispatch(int ctx_id, HTTP_AUTH_INFO& auth_info, const void* data, uint64_t len,
                                                bool& enableLog) try
{
	std::unique_ptr<std::lock_guard<std::mutex>> lockProxy;
	if(debug)
	{
		if(debug->flags & DebugCtx::FL_LOCK)
			lockProxy.reset(new std::lock_guard(debug->requestLock));
		if(debug->flags & DebugCtx::FL_RATELIMIT)
		{
			auto now = std::chrono::high_resolution_clock::now();
			std::this_thread::sleep_for(debug->last-now+debug->minRequestTime);
			debug->last = now;
		}
	}
	enableLog = false;
	using namespace std::string_literals;
	EWSContext context(ctx_id, auth_info, static_cast<const char*>(data), len, *this);
	if(!rpc_new_stack())
		mlog(LV_WARN, "[ews#%d]: Failed to allocate stack, exmdb might not work", ctx_id);
	auto cl0 = make_scope_exit([]{rpc_free_stack();});
	if(request_logging >= 2)
	{
		for(const XMLElement* xml = context.request.body->FirstChildElement(); xml; xml = xml->NextSiblingElement())
			enableLog = enableLog || logEnabled(xml->Name());
		if(enableLog)
			mlog(LV_DEBUG, "[ews#%d] Incoming data: %.*s", ctx_id,  len > INT_MAX ? INT_MAX : static_cast<int>(len),
			     static_cast<const char *>(data));
	}
	for(XMLElement* xml = context.request.body->FirstChildElement(); xml; xml = xml->NextSiblingElement())
	{
		bool logThis = logEnabled(xml->Name());
		enableLog = enableLog || logThis;
		XMLElement* responseContainer = context.response.body->InsertNewChildElement(xml->Name());
		responseContainer->SetAttribute("xmlns:m", Structures::NS_EWS_Messages::NS_URL);
		responseContainer->SetAttribute("xmlns:t", Structures::NS_EWS_Types::NS_URL);
		if(logThis && request_logging)
			mlog(LV_DEBUG, "[ews#%d] Processing %s", ctx_id,  xml->Name());
		auto handler = requestMap.find(xml->Name());
		if(handler == requestMap.end())
		    throw Exceptions::UnknownRequestError("Unknown request '"s+xml->Name()+"'.");
		else
			handler->second(xml, responseContainer, context);
	}
	XMLPrinter printer(nullptr, !pretty_response);
	context.response.doc.Print(&printer);
	return {printer.CStr(), 200};
} catch (const Exceptions::InputError &err) {
	return {SOAP::Envelope::fault("Client", err.what()), 200};
} catch (const std::exception &err) {
	return {SOAP::Envelope::fault("Server", err.what()), 500};
}

/**
 * @brief     Check if logging is enabled for this request
 *
 * @param     requestName  Name of the request
 *
 * @return    true if logging is enabled, false otherwise
 */
bool EWSPlugin::logEnabled(const std::string_view& requestName) const
{return std::binary_search(logFilters.begin(), logFilters.end(), requestName) != invertFilter;}

EWSPlugin::EWSPlugin() :
	mimePool(MIME_POOL::create(std::clamp(16*get_context_num(), 1024u, 16*1024u), 16, "ews_mime_pool"))
{
	loadConfig();
	cache.run(cache_interval);
}

/**
 * @brief      Initialize mysql adaptor function pointers
 */
EWSPlugin::_mysql::_mysql()
{
#define getService(f) \
	if (query_service2(# f, f) == nullptr) \
		throw std::runtime_error("[ews]: failed to get the \""# f"\" service")

	getService(get_domain_ids);
	getService(get_domain_info);
	getService(get_homedir);
	getService(get_maildir);
	getService(get_user_aliases);
	getService(get_user_displayname);
	getService(get_user_ids);
	getService(get_user_properties);
	getService(get_username_from_id);
#undef getService
}

EWSPlugin::_exmdb::_exmdb()
{
#define EXMIDL(n, p) do { \
	query_service2("exmdb_client_" #n, n); \
	if ((n) == nullptr) { \
		throw std::runtime_error("[ews]: failed to get the \"exmdb_client_"# n"\" service\n"); \
	} \
} while (false);
#define IDLOUT
#include <gromox/exmdb_idef.hpp>
#undef EXMIDL
#undef IDLOUT
}

static constexpr cfg_directive x500_defaults[] = {
	{"x500_org_name", "Gromox default"},
	CFG_TABLE_END,
};

static constexpr cfg_directive ews_cfg_defaults[] = {
	{"ews_experimental", "0", CFG_BOOL},
	{"ews_log_filter", "!"},
	{"ews_pretty_response", "0", CFG_BOOL},
	{"ews_request_logging", "0"},
	{"ews_response_logging", "0"},
	{"smtp_server_ip", "::1"},
	{"smtp_server_port", "25"},
	CFG_TABLE_END,
};

/**
 * @brief      Load configuration file
 */
void EWSPlugin::loadConfig()
{
	auto cfg = config_file_initd("exmdb_provider.cfg", get_config_path(), x500_defaults);
	if(!cfg)
	{
		mlog(LV_INFO, "[ews]: Failed to load config file");
		return;
	}
	x500_org_name = cfg->get_value("x500_org_name");
	mlog(LV_INFO, "[ews]: x500 org name is \"%s\"", x500_org_name.c_str());

	cfg = config_file_initd("ews.cfg", get_config_path(), ews_cfg_defaults);
	cfg->get_int("ews_experimental", &experimental);
	cfg->get_int("ews_pretty_response", &pretty_response);
	cfg->get_int("ews_request_logging", &request_logging);
	cfg->get_int("ews_response_logging", &response_logging);

	int temp;
	if(cfg->get_int("ews_cache_interval", &temp))
		cache_interval = std::chrono::milliseconds(temp);
	if(cfg->get_int("ews_cache_attachment_instance_lifetime", &temp))
		cache_attachment_instance_lifetime = std::chrono::milliseconds(temp);
	if(cfg->get_int("ews_cache_message_instance_lifetime", &temp))
		cache_message_instance_lifetime = std::chrono::milliseconds(temp);

	smtp_server_ip = cfg->get_value("smtp_server_ip");
	if(cfg->get_int("smtp_server_port", &temp))
		smtp_server_port = uint16_t(temp);

	const char* logFilter = cfg->get_value("ews_log_filter");
	if(logFilter && strlen(logFilter))
	{
		invertFilter = *logFilter == '!';
		logFilter += invertFilter;
		for(const char* sep = strchr(logFilter, ','); sep != nullptr; logFilter = ++sep, sep = strchr(sep, ','))
			logFilters.emplace_back(std::string_view(logFilter, sep-logFilter));
		if(*logFilter)
			logFilters.emplace_back(logFilter);
		std::sort(logFilters.begin(), logFilters.end());
	}
	const char* debugOpts = cfg->get_value("ews_debug");
	if(debugOpts)
		debug.reset(new DebugCtx(debugOpts));
}

///////////////////////////////////////////////////////////////////////////////
//Plugin management

static std::unique_ptr<EWSPlugin> g_ews_plugin; ///< Current plugin

/**
 * @brief      Initialize plugin
 *
 * @param      apidata  HPM API data
 *
 * @return     TRUE if initialization was successful, false otherwise
 */
static BOOL ews_init(void **apidata)
{
	LINK_HPM_API(apidata)
	HPM_INTERFACE ifc{};
	ifc.preproc = &EWSPlugin::preproc;
	ifc.proc    = [](int ctx, const void *cont, uint64_t len) { return g_ews_plugin->proc(ctx, cont, len); };
	ifc.retr    = [](int) {return HPM_RETRIEVE_DONE;};
	ifc.term    = [](int) {};
	if (!register_interface(&ifc))
		return false;
	try {
		g_ews_plugin.reset(new EWSPlugin());
	} catch (const std::exception &e) {
		mlog(LV_ERR, "[ews] failed to initialize plugin: %s", e.what());
		return false;
	}
	return TRUE;
}

/**
 * @brief      Plugin main function
 *
 * Used for (de-)initializing the plugin
 *
 * @param      reason  Reason the function is calles
 * @param      data    Additional, reason specific data
 *
 * @return     TRUE if successful, false otherwise
 */
static BOOL ews_main(int reason, void **data)
{
	if (reason == PLUGIN_INIT)
		return ews_init(data);
	else if(reason == PLUGIN_FREE)
		g_ews_plugin.reset();
	return TRUE;
}

HPM_ENTRY(ews_main);

///////////////////////////////////////////////////////////////////////////////////////////////////
//Cache


EWSPlugin::ExmdbInstance::ExmdbInstance(const EWSPlugin& p, const std::string& d, uint32_t i) :
	plugin(p), dir(d), instanceId(i)
{}

/**
 * @brief     Unload instance
 */
EWSPlugin::ExmdbInstance::~ExmdbInstance()
{plugin.exmdb.unload_instance(dir.c_str(), instanceId);}

/**
 * @brief      Load message instance
 *
 * @param      dir   Home directory of user or domain
 * @param      fid   Parent folder ID
 * @param      mid   Message ID
 *
 * @return     Message instance information
 */
std::shared_ptr<EWSPlugin::ExmdbInstance> EWSPlugin::loadMessageInstance(const std::string& dir, uint64_t fid,
                                                                         uint64_t mid) const
{
	detail::MessageInstanceKey mkey{dir, mid};
	try {
		return std::get<std::shared_ptr<EWSPlugin::ExmdbInstance>>(cache.get(mkey, cache_message_instance_lifetime));
	} catch(const std::out_of_range&) {
	}
	uint32_t instanceId;
	if(!exmdb.load_message_instance(dir.c_str(), nullptr, CP_ACP, false,fid, mid, &instanceId))
		throw DispatchError(Exceptions::E3077);
	std::shared_ptr<ExmdbInstance> instance(new ExmdbInstance(*this, dir, instanceId));
	cache.emplace(cache_message_instance_lifetime, mkey, instance);
	return instance;
}

/**
 * @brief      Load attachment instance
 *
 * @param      dir   Home directory of user or domain
 * @param      fid   Parent folder ID
 * @param      mid   Message ID
 * @param      aid   Attachment ID
 *
 * @return     Attachment instance information
 */
std::shared_ptr<EWSPlugin::ExmdbInstance> EWSPlugin::loadAttachmentInstance(const std::string& dir, uint64_t fid,
                                                                            uint64_t mid, uint32_t aid) const
{
	detail::AttachmentInstanceKey akey{dir, mid, aid};
	try {
		return std::get<std::shared_ptr<EWSPlugin::ExmdbInstance>>(cache.get(akey, cache_attachment_instance_lifetime));
	} catch(const std::out_of_range&) {
	}
	auto messageInstance = loadMessageInstance(dir, fid, mid);
	uint32_t instanceId;
	if(!exmdb.load_attachment_instance(dir.c_str(), messageInstance->instanceId, aid, &instanceId))
		throw DispatchError(Exceptions::E3078);
	std::shared_ptr<ExmdbInstance> instance(new ExmdbInstance(*this, dir, instanceId));
	cache.emplace(cache_message_instance_lifetime, akey, instance);
	return instance;
}

///////////////////////////////////////////////////////////////////////////////
// Hashing

template<>
inline uint64_t FNV::operator()(const std::string& str) noexcept
{return operator()(str.data(), str.size());}

size_t std::hash<detail::AttachmentInstanceKey>::operator()(const detail::AttachmentInstanceKey& key) const noexcept
{return FNV(key.dir, key.mid, key.aid).value;}

size_t std::hash<detail::MessageInstanceKey>::operator()(const detail::MessageInstanceKey& key) const noexcept
{return FNV(key.dir, key.mid).value;}
