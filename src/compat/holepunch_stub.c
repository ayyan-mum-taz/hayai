// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Stand-in for libchiaki's remote/holepunch.c.
//
// holepunch.c implements PSN-mediated NAT traversal for playing outside the
// local network. It is the only file in libchiaki that needs libcurl, json-c,
// miniupnpc and libevent, and hayai targets LAN sessions, so it is replaced by
// these five stubs. session.c and ctrl.c only ever reach them when a session
// was created with a non-NULL holepunch_session, which hayai never does.
//
// Restoring PSN remote connect means building the real remote/holepunch.c
// instead of this file and linking those four libraries; nothing else changes.

#include <arpa/inet.h>	// holepunch.h uses INET6_ADDRSTRLEN without including it
#include <chiaki/sock.h>
#include <chiaki/remote/holepunch.h>

CHIAKI_EXPORT ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession session)
{
	(void)session;
	ChiakiHolepunchRegistInfo info = { 0 };
	return info;
}

CHIAKI_EXPORT chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession session, ChiakiHolepunchPortType type)
{
	(void)session;
	(void)type;
	return NULL;
}

CHIAKI_EXPORT ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession session)
{
	(void)session;
	return CHIAKI_ERR_UNKNOWN;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession session, ChiakiHolepunchPortType port_type)
{
	(void)session;
	(void)port_type;
	return CHIAKI_ERR_UNKNOWN;
}

CHIAKI_EXPORT void chiaki_holepunch_session_fini(ChiakiHolepunchSession session)
{
	(void)session;
}

CHIAKI_EXPORT void chiaki_get_ps_selected_addr(ChiakiHolepunchSession session, char *ps_ip)
{
	(void)session;
	if(ps_ip)
		ps_ip[0] = '\0';
}

CHIAKI_EXPORT uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession session)
{
	(void)session;
	return 0;
}
