// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2021 grommunio GmbH
// This file is part of Gromox.
#include <cstdint>
#include <list>
#include <gromox/exmdb_client.hpp>
#include <gromox/exmdb_common_util.hpp>
#include <gromox/exmdb_ext.hpp>
#include <gromox/exmdb_provider_client.hpp>
#include <gromox/exmdb_rpc.hpp>
#include <gromox/exmdb_server.hpp>

using namespace gromox;

static void buildenv(const remote_svr &s)
{
	auto flags = s.type == EXMDB_ITEM::EXMDB_PRIVATE ? EM_PRIVATE : 0;
	exmdb_server::build_env(flags, nullptr);
}

int exmdb_client_run_front(const char *dir)
{
	return exmdb_client_run(dir, EXMDB_CLIENT_ALLOW_DIRECT | EXMDB_CLIENT_ASYNC_CONNECT,
	       buildenv, exmdb_server::free_env, exmdb_server::event_proc);
}

/* Caution. This function is not a common exmdb service,
	it only can be called by message_rule_new_message to
	pass a message to the delegate's mailbox. */
BOOL exmdb_client_relay_delivery(const char *dir, const char *from_address,
    const char *account, cpid_t cpid, const MESSAGE_CONTENT *pmsg,
	const char *pdigest, uint32_t *presult)
{
	BOOL b_private;
	
	if (exmdb_client_check_local(dir, &b_private)) {
		auto original_dir = exmdb_server::get_dir();
		exmdb_server::set_dir(dir);
		uint64_t folder_id = 0, msg_id = 0;
		auto b_result = exmdb_server::deliver_message(dir, from_address,
		                account, cpid,
		                DELIVERY_DO_RULES | DELIVERY_DO_NOTIF,
		                pmsg, pdigest, &folder_id, &msg_id, presult);
		exmdb_server::set_dir(original_dir);
		return b_result;
	}
	exreq_deliver_message q{exmdb_callid::deliver_message, deconst(dir),
		deconst(from_address), deconst(account), cpid, 0,
		deconst(pmsg), deconst(pdigest)};
	exresp_deliver_message r{};
	if (!exmdb_client_do_rpc(&q, &r))
		return FALSE;
	*presult = r.result;
	return TRUE;
}
