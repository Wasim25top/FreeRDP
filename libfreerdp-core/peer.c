/**
 * FreeRDP: A Remote Desktop Protocol client.
 * RDP Server Peer
 *
 * Copyright 2011 Vic Lee
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "peer.h"

static boolean freerdp_peer_initialize(freerdp_peer* client)
{
	client->context->rdp->settings->server_mode = True;
	client->context->rdp->state = CONNECTION_STATE_INITIAL;

	return True;
}

static boolean freerdp_peer_get_fds(freerdp_peer* client, void** rfds, int* rcount)
{
	rfds[*rcount] = (void*)(long)(client->context->rdp->transport->tcp->sockfd);
	(*rcount)++;

	return True;
}

static boolean freerdp_peer_check_fds(freerdp_peer* client)
{
	rdpRdp* rdp;
	int status;

	rdp = client->context->rdp;

	status = rdp_check_fds(rdp);
	if (status < 0)
		return False;

	return True;
}

static boolean peer_recv_data_pdu(freerdp_peer* client, STREAM* s)
{
	uint8 type;
	uint16 length;
	uint32 share_id;
	uint8 compressed_type;
	uint16 compressed_len;


	if (!rdp_read_share_data_header(s, &length, &type, &share_id, &compressed_type, &compressed_len))
		return False;

	switch (type)
	{
		case DATA_PDU_TYPE_SYNCHRONIZE:
			if (!rdp_recv_client_synchronize_pdu(s))
				return False;
			break;

		case DATA_PDU_TYPE_CONTROL:
			if (!rdp_server_accept_client_control_pdu(client->context->rdp, s))
				return False;
			break;

		case DATA_PDU_TYPE_BITMAP_CACHE_PERSISTENT_LIST:
			/* TODO: notify server bitmap cache data */
			break;

		case DATA_PDU_TYPE_FONT_LIST:
			if (!rdp_server_accept_client_font_list_pdu(client->context->rdp, s))
				return False;
			if (client->PostConnect)
			{
				if (!client->PostConnect(client))
					return False;
				/**
				 * PostConnect should only be called once and should not be called
				 * after a reactivation sequence.
				 */
				client->PostConnect = NULL;
			}
			if (client->Activate)
			{
				/* Activate will be called everytime after the client is activated/reactivated. */
				if (!client->Activate(client))
					return False;
			}
			break;

		case DATA_PDU_TYPE_SHUTDOWN_REQUEST:
			mcs_send_disconnect_provider_ultimatum(client->context->rdp->mcs);
			return False;

		default:
			printf("Data PDU type %d\n", type);
			break;
	}

	return True;
}

static boolean peer_recv_tpkt_pdu(freerdp_peer* client, STREAM* s)
{
	uint16 length;
	uint16 pduType;
	uint16 pduLength;
	uint16 channelId;

	if (!rdp_read_header(client->context->rdp, s, &length, &channelId))
	{
		printf("Incorrect RDP header.\n");
		return False;
	}

	if (channelId != MCS_GLOBAL_CHANNEL_ID)
	{
		/* TODO: process channel data from client */
	}
	else
	{
		if (!rdp_read_share_control_header(s, &pduLength, &pduType, &client->settings->pdu_source))
			return False;

		switch (pduType)
		{
			case PDU_TYPE_DATA:
				if (!peer_recv_data_pdu(client, s))
					return False;
				break;

			default:
				printf("Client sent pduType %d\n", pduType);
				return False;
		}
	}

	return True;
}

static boolean peer_recv_fastpath_pdu(freerdp_peer* client, STREAM* s)
{
	uint16 length;
	rdpRdp* rdp;
	rdpFastPath* fastpath;

	rdp = client->context->rdp;
	fastpath = rdp->fastpath;
	length = fastpath_read_header_rdp(fastpath, s);

	if (length == 0 || length > stream_get_left(s))
	{
		printf("incorrect FastPath PDU header length %d\n", length);
		return False;
	}

	if (fastpath->encryptionFlags & FASTPATH_OUTPUT_ENCRYPTED)
	{
		rdp_decrypt(rdp, s, length);
	}

	return fastpath_recv_inputs(fastpath, s);
}

static boolean peer_recv_pdu(freerdp_peer* client, STREAM* s)
{
	if (tpkt_verify_header(s))
		return peer_recv_tpkt_pdu(client, s);
	else
		return peer_recv_fastpath_pdu(client, s);
}

static boolean peer_recv_callback(rdpTransport* transport, STREAM* s, void* extra)
{
	freerdp_peer* client = (freerdp_peer*) extra;

	switch (client->context->rdp->state)
	{
		case CONNECTION_STATE_INITIAL:
			if (!rdp_server_accept_nego(client->context->rdp, s))
				return False;
			break;

		case CONNECTION_STATE_NEGO:
			if (!rdp_server_accept_mcs_connect_initial(client->context->rdp, s))
				return False;
			break;

		case CONNECTION_STATE_MCS_CONNECT:
			if (!rdp_server_accept_mcs_erect_domain_request(client->context->rdp, s))
				return False;
			break;

		case CONNECTION_STATE_MCS_ERECT_DOMAIN:
			if (!rdp_server_accept_mcs_attach_user_request(client->context->rdp, s))
				return False;
			break;

		case CONNECTION_STATE_MCS_ATTACH_USER:
			if (!rdp_server_accept_mcs_channel_join_request(client->context->rdp, s))
				return False;
			break;

		case CONNECTION_STATE_MCS_CHANNEL_JOIN:
			if (!rdp_server_accept_client_info(client->context->rdp, s))
				return False;
			break;

		case CONNECTION_STATE_LICENSE:
			if (!rdp_server_accept_confirm_active(client->context->rdp, s))
				return False;
			break;

		case CONNECTION_STATE_ACTIVE:
			if (!peer_recv_pdu(client, s))
				return False;
			break;

		default:
			printf("Invalid state %d\n", client->context->rdp->state);
			return False;
	}

	return True;
}

static void freerdp_peer_disconnect(freerdp_peer* client)
{
	transport_disconnect(client->context->rdp->transport);
}

void freerdp_peer_context_new(freerdp_peer* client)
{
	rdpRdp* rdp;

	rdp = rdp_new(NULL);
	client->input = rdp->input;
	client->update = rdp->update;
	client->settings = rdp->settings;

	client->context = (rdpContext*) xzalloc(client->context_size);
	client->context->rdp = rdp;
	client->context->peer = client;

	client->update->context = client->context;
	client->input->context = client->context;

	update_register_server_callbacks(client->update);

	transport_attach(rdp->transport, client->sockfd);

	rdp->transport->recv_callback = peer_recv_callback;
	rdp->transport->recv_extra = client;
	transport_set_blocking_mode(rdp->transport, False);

	IFCALL(client->ContextNew, client, client->context);
}

void freerdp_peer_context_free(freerdp_peer* client)
{
	IFCALL(client->ContextFree, client, client->context);
}

freerdp_peer* freerdp_peer_new(int sockfd)
{
	freerdp_peer* client;

	client = xnew(freerdp_peer);

	if (client != NULL)
	{
		client->sockfd = sockfd;
		client->context_size = sizeof(rdpContext);
		client->Initialize = freerdp_peer_initialize;
		client->GetFileDescriptor = freerdp_peer_get_fds;
		client->CheckFileDescriptor = freerdp_peer_check_fds;
		client->Disconnect = freerdp_peer_disconnect;
	}

	return client;
}

void freerdp_peer_free(freerdp_peer* client)
{
	if (client)
	{
		rdp_free(client->context->rdp);
		xfree(client);
	}
}

