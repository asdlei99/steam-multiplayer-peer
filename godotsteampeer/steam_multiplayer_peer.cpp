#include <godot_cpp/core/class_db.hpp>

#include "steam_multiplayer_peer.h"

#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// Define Steam API constants
#define STEAM_BUFFER_SIZE 255

SteamMultiplayerPeer::SteamMultiplayerPeer() :
		// Networking Sockets callbacks /////////////
		callback_network_connection_status_changed(this, &SteamMultiplayerPeer::network_connection_status_changed) {
}

SteamMultiplayerPeer::~SteamMultiplayerPeer() {
	if (_is_active()) {
		close();
	}
}

Error SteamMultiplayerPeer::_get_packet(const uint8_t **r_buffer, int32_t *r_buffer_size) {
	ERR_FAIL_COND_V_MSG(incoming_packets.size() == 0, ERR_UNAVAILABLE, "No incoming packets available.");

	delete next_received_packet;
	next_received_packet = incoming_packets.front()->get();

	*r_buffer_size = next_received_packet->size;
	*r_buffer = (const uint8_t *)(&next_received_packet->data); //REVIEW A pointer to a reference? I feel like this is worthy of more consideration.
	incoming_packets.pop_front();

	return OK;
}

// REVIEW many differences for EnetPeer
Error SteamMultiplayerPeer::_put_packet(const uint8_t *p_buffer, int32_t p_buffer_size) {
	ERR_FAIL_COND_V_MSG(!_is_active(), ERR_UNCONFIGURED, "The multiplayer instance isn't currently active.");
	ERR_FAIL_COND_V_MSG(connection_status != CONNECTION_CONNECTED, ERR_UNCONFIGURED, "The multiplayer instance isn't currently connected to any server or client.");
	ERR_FAIL_COND_V_MSG(target_peer != 0 && !peerId_to_steamId.has(ABS(target_peer)), ERR_INVALID_PARAMETER, vformat("Invalid target peer: %d", target_peer));
	ERR_FAIL_COND_V(active_mode == MODE_CLIENT && !peerId_to_steamId.has(1), ERR_BUG);
	int transferMode = _get_steam_transfer_flag();

	if(p_buffer_size > 0)
	{
		String print_msg;
		for (size_t i = 0; i < p_buffer_size; i++)
		{
			int v = p_buffer[i];
			print_msg += "[";
			print_msg += String::num_uint64(v, 16, true);
			print_msg += "] ";
		}
		UtilityFunctions::print("Packet = ", print_msg);
	}

	if (target_peer == 0) {
		Error returnValue = OK;
		for (HashMap<int64_t, Ref<SteamConnection>>::Iterator E = connections_by_steamId64.begin(); E; ++E) {
			SteamConnection::Packet *packet = new SteamConnection::Packet(p_buffer, p_buffer_size, transferMode);
			Error errorCode = E->value->send(packet);
			if (errorCode != OK) {
				// DEBUG_DATA_SIGNAL_V("put_packet failed.", errorCode);
				returnValue = errorCode;
			}
		}
		return returnValue;
	} else {
		SteamConnection::Packet *packet = new SteamConnection::Packet(p_buffer, p_buffer_size, transferMode);
		return get_connection_by_peer(target_peer)->send(packet);
	}
}

int32_t SteamMultiplayerPeer::_get_available_packet_count() const {
	int32_t size = incoming_packets.size();
	return size;
}

int32_t SteamMultiplayerPeer::_get_max_packet_size() const {
	return k_cbMaxSteamNetworkingSocketsMessageSizeSend;
}

int32_t SteamMultiplayerPeer::_get_packet_channel() const {
	return 0;
}

MultiplayerPeer::TransferMode SteamMultiplayerPeer::_get_packet_mode() const {
	ERR_FAIL_COND_V_MSG(!_is_active(), TRANSFER_MODE_RELIABLE, "The multiplayer instance isn't currently active.");
	ERR_FAIL_COND_V_MSG(incoming_packets.size() == 0, TRANSFER_MODE_RELIABLE, "No pending packets, cannot get transfer mode.");

	if (incoming_packets.front()->get()->transfer_mode & k_nSteamNetworkingSend_Reliable) {
		return TRANSFER_MODE_RELIABLE;
	} else {
		return TRANSFER_MODE_UNRELIABLE;
	}
}

void SteamMultiplayerPeer::_set_transfer_channel(int32_t p_channel) {
}

int32_t SteamMultiplayerPeer::_get_transfer_channel() const {
	return 0;
}

void SteamMultiplayerPeer::_set_transfer_mode(MultiplayerPeer::TransferMode p_mode) {
	transfer_mode = p_mode;
}

MultiplayerPeer::TransferMode SteamMultiplayerPeer::_get_transfer_mode() const {
	return transfer_mode;
}

void SteamMultiplayerPeer::_set_target_peer(int32_t p_peer) {
	target_peer = p_peer;
}

int32_t SteamMultiplayerPeer::_get_packet_peer() const {
	ERR_FAIL_COND_V_MSG(!_is_active(), 1, "The multiplayer instance isn't currently active.");
	ERR_FAIL_COND_V_MSG(incoming_packets.size() == 0, 1, "No packets to receive.");

	int32_t peer_id = connections_by_steamId64[incoming_packets.front()->get()->sender.to_int()]->peer_id;
	return peer_id;
}

bool SteamMultiplayerPeer::_is_server() const {
	return unique_id == 1;
}

#define MAX_MESSAGE_COUNT 255
void SteamMultiplayerPeer::_poll() {
	ERR_FAIL_COND_MSG(!_is_active(), "The multiplayer instance isn't currently active.");

	SteamNetworkingMessage_t *messages[MAX_MESSAGE_COUNT];

	for (HashMap<int64_t, Ref<SteamConnection>>::ConstIterator E = connections_by_steamId64.begin(); E; ++E) {
		int64_t key = E->key;
		Ref<SteamConnection> value = E->value;
		int count = SteamNetworkingSockets()->ReceiveMessagesOnConnection(value->steam_connection, messages, MAX_MESSAGE_COUNT);
		if (count > 0) {
			UtilityFunctions::print("Received (",count,") messages!");
			for (int i = 0; i < count; i++) {
				SteamNetworkingMessage_t *msg = messages[i];
				
				if (get_peer_id_from_steam64(msg->m_identityPeer.GetSteamID64()) != -1) {
					UtilityFunctions::print("- Received Base message");
					_process_message(msg);
				} else {
					UtilityFunctions::print("- Received Ping message");
					_process_ping(msg);
				}
				msg->Release();
			}
		}
	}
}

void SteamMultiplayerPeer::_close() {
	if (!_is_active()) {
		return;
	}
	if (connection_status != CONNECTION_CONNECTED) {
		return;
	}

	for (HashMap<int64_t, Ref<SteamConnection>>::ConstIterator E = connections_by_steamId64.begin(); E; ++E) {
		const Ref<SteamConnection> connection = E->value;
		// TODO On Enet disconnect all peers with
		// peer_disconnect_now(0);
		close_connection(connection);
	}

	if (_is_server()) {
		close_listen_socket();
	}

	peerId_to_steamId.clear();
	connections_by_steamId64.clear();
	active_mode = MODE_NONE;
	unique_id = 0;
	connection_status = CONNECTION_DISCONNECTED;
	steam_id.set_from_CSteamID(CSteamID()); // = SteamID();
}

void SteamMultiplayerPeer::_disconnect_peer(int32_t p_peer, bool p_force) {
	ERR_FAIL_COND(!_is_active() || !peerId_to_steamId.has(p_peer));
	Ref<SteamConnection> connection = get_connection_by_peer(p_peer);
	close_connection(connection);

	connection->flush();
	// peerId_to_steamId[p_peer]->peer_disconnect(0); // Will be removed during next poll.
	if (active_mode == MODE_CLIENT || active_mode == MODE_SERVER) {
		get_connection_by_peer(0)->flush();
	} else {
		// ERR_FAIL_COND(!hosts.has(p_peer));
		// hosts[p_peer]->flush();
	}
	if (p_force) {
		// peers.erase(p_peer);
		// if (hosts.has(p_peer)) {
		// 	hosts.erase(p_peer);
		// }
		if (active_mode == MODE_CLIENT) {
			connections_by_steamId64.clear(); // Avoid flushing again.
			close();
		}
	}
}

int32_t SteamMultiplayerPeer::_get_unique_id() const {
	ERR_FAIL_COND_V_MSG(!_is_active(), 0, "The multiplayer instance isn't currently active.");
	return unique_id;
}

bool SteamMultiplayerPeer::_is_server_relay_supported() const {
	return active_mode == MODE_SERVER || active_mode == MODE_CLIENT;
}

MultiplayerPeer::ConnectionStatus SteamMultiplayerPeer::_get_connection_status() const {
	return connection_status;
}

bool SteamMultiplayerPeer::close_listen_socket() {
	if (SteamNetworkingSockets() == NULL) {
		UtilityFunctions::printerr("SteamNetworkingSockets is null!");
		return false;
	}
	if (!SteamNetworkingSockets()->CloseListenSocket(listen_socket)) {
		UtilityFunctions::printerr("Fail to close listen socket ", listen_socket);
		return false;
	}
	UtilityFunctions::print("Success for close listen socket ", listen_socket);
	return true;
}

bool SteamMultiplayerPeer::close_connection(const Ref<SteamConnection> connection) {
	if (SteamNetworkingSockets() == NULL) {
		UtilityFunctions::printerr("SteamNetworkingSockets is null!");
		return false;
	}
	if (connection->steam_connection == k_HSteamNetConnection_Invalid) {
		UtilityFunctions::printerr("Steam Connection is invalid!");
		return false;
	}
	if (!SteamNetworkingSockets()->CloseConnection(connection->steam_connection, ESteamNetConnectionEnd::k_ESteamNetConnectionEnd_App_Generic, "Failed to accept connection", false)) {
		UtilityFunctions::printerr("Fail to close connection ", connection);
		return false;
	}
	UtilityFunctions::print("Success for close connection socket ", connection);
	return true;
}

void DebugOutputFunction(ESteamNetworkingSocketsDebugOutputType nType, const char *pszMsg) {
	UtilityFunctions::print(pszMsg);
}

Error SteamMultiplayerPeer::create_host(int n_local_virtual_port, Array options) {
	ERR_FAIL_COND_V_MSG(_is_active(), ERR_ALREADY_IN_USE, "The multiplayer instance is already active.");
	if (SteamNetworkingSockets() == NULL) {
		return Error::ERR_CANT_CREATE;
	}

	SteamNetworkingUtils()->InitRelayNetworkAccess();

	// SteamNetworkingUtils()->SetDebugOutputFunction(ESteamNetworkingSocketsDebugOutputType::k_ESteamNetworkingSocketsDebugOutputType_Everything, DebugOutputFunction);

	unique_id = 1;

	const SteamNetworkingConfigValue_t *these_options = convert_options_array(options);
	listen_socket = SteamNetworkingSockets()->CreateListenSocketP2P(n_local_virtual_port, options.size(), these_options);
	UtilityFunctions::print("create_host with ", listen_socket);
	delete[] these_options;
	if (listen_socket == k_HSteamListenSocket_Invalid) {
		unique_id = 0;
		return Error::ERR_CANT_CREATE;
	}
	active_mode = MODE_SERVER;
	connection_status = ConnectionStatus::CONNECTION_CONNECTED;
	return Error::OK;
}

// REVIEW Problem with godot steam reference (in godot steam use const String& network_identity)
Error SteamMultiplayerPeer::create_client(uint64_t identity_remote, int n_remote_virtual_port, Array options) {
	ERR_FAIL_COND_V_MSG(_is_active(), ERR_ALREADY_IN_USE, "The multiplayer instance is already active.");
	if (SteamNetworkingSockets() == NULL) {
		return Error::ERR_CANT_CONNECT;
	}
	unique_id = generate_unique_id();
	UtilityFunctions::print("My unique id is = ",unique_id);
	// TODO Add peer_id to options for connection

	SteamNetworkingUtils()->InitRelayNetworkAccess();

	// Array peer_id_array_config_info;
	// peer_id_array_config_info.resize(3);
	// peer_id_array_config_info[0] = Variant(k_ESteamNetworkingConfig_ConnectionUserData);
	// peer_id_array_config_info[1] = Variant(2);
	// peer_id_array_config_info[2] = Variant(unique_id);
	// options.append(peer_id_array_config_info);

	const SteamNetworkingConfigValue_t *these_options = convert_options_array(options);

	SteamNetworkingIdentity p_remote_id;
	p_remote_id.SetSteamID64(identity_remote);

	connection = SteamNetworkingSockets()->ConnectP2P(p_remote_id, n_remote_virtual_port, options.size(), these_options);

	delete[] these_options;
	if (connection == k_HSteamNetConnection_Invalid) {
		unique_id = 0;
		return Error::ERR_CANT_CREATE;
	}

	active_mode = MODE_CLIENT;
	connection_status = ConnectionStatus::CONNECTION_CONNECTING;
	UtilityFunctions::print(connection, " p_remote_id=", p_remote_id.GetSteamID64());
	return Error::OK;
}

bool SteamMultiplayerPeer::get_identity(SteamNetworkingIdentity *p_identity) {
	return SteamNetworkingSockets()->GetIdentity(p_identity);
}

void SteamMultiplayerPeer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create_host", "n_local_virtual_port", "options"), &SteamMultiplayerPeer::create_host);
	ClassDB::bind_method(D_METHOD("create_client", "identity_remote", "n_local_virtual_port", "options"), &SteamMultiplayerPeer::create_client);
	ClassDB::bind_method(D_METHOD("set_listen_socket", "listen_socket"), &SteamMultiplayerPeer::set_listen_socket);
	ClassDB::bind_method(D_METHOD("get_listen_socket"), &SteamMultiplayerPeer::get_listen_socket);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "listen_socket"), "set_listen_socket", "get_listen_socket");

	// NETWORKING SOCKETS SIGNALS ///////////////
	ADD_SIGNAL(MethodInfo("network_connection_status_changed", PropertyInfo(Variant::INT, "connect_handle"), PropertyInfo(Variant::DICTIONARY, "connection"), PropertyInfo(Variant::INT, "old_state")));
}

const int SteamMultiplayerPeer::_get_steam_transfer_flag() {
	MultiplayerPeer::TransferMode transfer_mode = get_transfer_mode();

	int32_t flags = (k_nSteamNetworkingSend_NoNagle * no_nagle) | (k_nSteamNetworkingSend_NoDelay * no_delay); //interesting use

	switch (transfer_mode) {
		case TransferMode::TRANSFER_MODE_RELIABLE:
			return k_nSteamNetworkingSend_Reliable | flags;
			break;
		case TransferMode::TRANSFER_MODE_UNRELIABLE:
			return k_nSteamNetworkingSend_Unreliable | flags;
			break;
		case TransferMode::TRANSFER_MODE_UNRELIABLE_ORDERED:
			//Unreliable order not supported here!
			return k_nSteamNetworkingSend_Reliable | flags;
			break;
	}

	ERR_FAIL_V_MSG(-1, "Flags error. Switch fallthrough in _get_steam_transfer_flag");
}

// NETWORKING SOCKETS CALLBACKS /////////////////
//
//! This callback is posted whenever a connection is created, destroyed, or
//! changes state. The m_info field will contain a complete description of the
//! connection at the time the change occurred and the callback was posted. In
//! particular, m_info.m_eState will have the new connection state.
void SteamMultiplayerPeer::network_connection_status_changed(SteamNetConnectionStatusChangedCallback_t *call_data) {
	// Connection handle.
	uint64_t connect_handle = call_data->m_hConn;

	// Full connection info.
	SteamNetConnectionInfo_t connection_info = call_data->m_info;

	// Move connection info into a dictionary
	Dictionary connection;
	char identity[STEAM_BUFFER_SIZE];
	connection_info.m_identityRemote.ToString(identity, STEAM_BUFFER_SIZE);
	connection["identity"] = identity;
	connection["user_data"] = (uint64_t)connection_info.m_nUserData;
	connection["listen_socket"] = connection_info.m_hListenSocket;
	char ip_address[STEAM_BUFFER_SIZE];
	connection_info.m_addrRemote.ToString(ip_address, STEAM_BUFFER_SIZE, true);
	connection["remote_address"] = ip_address;
	connection["remote_pop"] = connection_info.m_idPOPRemote;
	connection["pop_relay"] = connection_info.m_idPOPRelay;
	connection["connection_state"] = connection_info.m_eState;
	connection["end_reason"] = connection_info.m_eEndReason;
	connection["end_debug"] = connection_info.m_szEndDebug;
	connection["debug_description"] = connection_info.m_szConnectionDescription;
	int old_state = call_data->m_eOldState;
	//emit_signal("network_connection_status_changed", connect_handle, connection, old_state);

	// A new connection arrives on a listen socket.
	if (connection_info.m_hListenSocket && call_data->m_eOldState == ESteamNetworkingConnectionState::k_ESteamNetworkingConnectionState_None && call_data->m_info.m_eState == ESteamNetworkingConnectionState::k_ESteamNetworkingConnectionState_Connecting) {
		UtilityFunctions::print("Trying AcceptConnection...");
		EResult res = SteamNetworkingSockets()->AcceptConnection(connect_handle);
		if (res != k_EResultOK) {
			UtilityFunctions::print("AcceptConnection error! returned", res);
			SteamNetworkingSockets()->CloseConnection(connect_handle, k_ESteamNetConnectionEnd_AppException_Generic, "Failed to accept connection", false);
			return;
		} else {
			UtilityFunctions::print("AcceptConnection success! User data =", connection_info.m_nUserData);
		}
	}

	// A connection you initiated has been accepted by the remote host.
	if ((call_data->m_eOldState == ESteamNetworkingConnectionState::k_ESteamNetworkingConnectionState_Connecting ||
				call_data->m_eOldState == ESteamNetworkingConnectionState::k_ESteamNetworkingConnectionState_FindingRoute) &&
			call_data->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) {

		SteamID steam_id = call_data->m_info.m_identityRemote.GetSteamID();
		add_connection(steam_id, call_data->m_hConn);

		if (_is_server()) {
			
			// Server correct allocated peer id for steam connection
			Error err = connections_by_steamId64[steam_id.to_int()]->ping();
		}
		else
		{
			UtilityFunctions::print("Connected to socket!");
			connection_status = ConnectionStatus::CONNECTION_CONNECTED;
		}
	}

	if (_is_server())
		return;

	/////// Client callbacks

	// A connection has been actively rejected or closed by the remote host.
	if ((call_data->m_eOldState == ESteamNetworkingConnectionState::k_ESteamNetworkingConnectionState_Connecting ||
				call_data->m_eOldState == ESteamNetworkingConnectionState::k_ESteamNetworkingConnectionState_Connected) &&
			call_data->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer) {
		emit_signal("peer_disconnected", (int32_t)1);
		close();
		return;
	}

	// A problem was detected with the connection, and it has been closed by the local host. The most common failure is timeout,
	if ((call_data->m_eOldState == ESteamNetworkingConnectionState::k_ESteamNetworkingConnectionState_Connecting ||
				call_data->m_eOldState == ESteamNetworkingConnectionState::k_ESteamNetworkingConnectionState_Connected) &&
			call_data->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
		emit_signal("peer_disconnected", (int32_t)1);
		//remove_connection_peer();
		return;
	}
}

// GODOT MULTIPLAYER PEER UTILS  ///////////////////
//
// Helper function to turn an array of options into an array of
// SteamNetworkingConfigValue_t structs
const SteamNetworkingConfigValue_t *SteamMultiplayerPeer::convert_options_array(Array options) {
	// Get the number of option arrays in the array.
	int options_size = options.size();
	// Create the array for options.
	SteamNetworkingConfigValue_t *option_array = new SteamNetworkingConfigValue_t[options_size];
	// If there are options
	if (options_size > 0) {
		// Populate the options
		for (int i = 0; i < options_size; i++) {
			SteamNetworkingConfigValue_t this_option;
			Array sent_option = options[i];
			// Get the configuration value.
			// This is a convoluted way of doing it but can't seem to cast the value
			// as an enum so here we are.
			ESteamNetworkingConfigValue this_value = ESteamNetworkingConfigValue((int)sent_option[0]);
			if ((int)sent_option[1] == 1) {
				this_option.SetInt32(this_value, sent_option[2]);
			} else if ((int)sent_option[1] == 2) {
				this_option.SetInt64(this_value, sent_option[2]);
			} else if ((int)sent_option[1] == 3) {
				this_option.SetFloat(this_value, sent_option[2]);
			} else if ((int)sent_option[1] == 4) {
				char *this_string = { 0 };
				String passed_string = sent_option[2];
				strcpy(this_string, passed_string.utf8().get_data());
				this_option.SetString(this_value, this_string);
			} else {
				Object *this_pointer;
				this_pointer = sent_option[2];
				this_option.SetPtr(this_value, this_pointer);
			}
			option_array[i] = this_option;
		}
	}
	return option_array;
}

Ref<SteamConnection> SteamMultiplayerPeer::get_connection_by_peer(int peer_id) {
	if (peerId_to_steamId.has(peer_id))
		return peerId_to_steamId[peer_id];

	return nullptr;
}

void SteamMultiplayerPeer::add_connection(const SteamID &steamId, HSteamNetConnection connection) {
	ERR_FAIL_COND_MSG(steamId == SteamUser()->GetSteamID(), "Cannot add self as a new peer.");

	Ref<SteamConnection> connection_data = Ref<SteamConnection>(memnew(SteamConnection(steamId)));
	connection_data->steam_connection = connection;
	connections_by_steamId64[steamId.to_int()] = connection_data;
}

void SteamMultiplayerPeer::_process_message(const SteamNetworkingMessage_t *msg) {
	ERR_FAIL_COND_MSG(msg->GetSize() > MAX_STEAM_PACKET_SIZE, "Packet too large to send,");

	SteamConnection::Packet *packet = new SteamConnection::Packet;
	packet->sender.set_from_CSteamID(msg->m_identityPeer.GetSteamID());
	packet->size = msg->GetSize();
	packet->transfer_mode = msg->m_nFlags;

	uint8_t *rawData = (uint8_t *)msg->GetData();
	memcpy(packet->data, rawData, packet->size);
	incoming_packets.push_back(packet);
}

void SteamMultiplayerPeer::_process_ping(const SteamNetworkingMessage_t *msg) {
	ERR_FAIL_COND_MSG(sizeof(SteamConnection::PingPayload) != msg->GetSize(), "Payload is the wrong size for a ping.");

	SteamConnection::PingPayload *receive = (SteamConnection::PingPayload *)msg->GetData();
	SteamID steam_id = msg->m_identityPeer.GetSteamID();

	if (receive->peer_id == -1) {
		// Client receive peer setup confirmation from server
		emit_signal("peer_connected", unique_id);


	} else {

		Ref<SteamConnection> connection = connections_by_steamId64[steam_id.to_int()];
		if (connection->peer_id == -1) {
			set_steam_id_peer(msg->m_identityPeer.GetSteamID(), receive->peer_id);
		}
		if(!_is_server())
		{
			SteamConnection::PingPayload payload = SteamConnection::PingPayload();
			payload.peer_id = unique_id;
			Error err = connections_by_steamId64[steam_id.to_int()]->ping(payload);
		}
		else
		{
			emit_signal("peer_connected", receive->peer_id);
		}
	}
}

uint64_t SteamMultiplayerPeer::get_steam64_from_peer_id(int peer) {
	if (peer == this->unique_id) {
		return SteamUser()->GetSteamID().ConvertToUint64();
	} else if (peerId_to_steamId.find(peer) == peerId_to_steamId.end()) {
		return peerId_to_steamId[peer]->steam_id.to_int();
	} else
		return -1;
}

int SteamMultiplayerPeer::get_peer_id_from_steam64(uint64_t steamid) {
	if (steamid == SteamUser()->GetSteamID().ConvertToUint64()) {
		return this->unique_id;
	} else if (connections_by_steamId64.has(steamid)) {
		return connections_by_steamId64[steamid]->peer_id;
	} else
		return -1;
}

//Should this be by reference?
int SteamMultiplayerPeer::get_peer_id_from_steam_id(SteamID &steamid) const {
	if (steamid == SteamID(SteamUser()->GetSteamID())) {
		return this->unique_id;
	} else if (connections_by_steamId64.has(steamid.to_int())) {
		return connections_by_steamId64[steamid.to_int()]->peer_id;
	} else
		return -1;
}

void SteamMultiplayerPeer::set_steam_id_peer(SteamID steam_id, int peer_id) {
	ERR_FAIL_COND_MSG(steam_id == SteamUser()->GetSteamID(), "Cannot add self as a new peer.");
	ERR_FAIL_COND_MSG(connections_by_steamId64.has(steam_id.to_int()) == false, "Steam ID missing");

	Ref<SteamConnection> con = connections_by_steamId64[steam_id.to_int()];
	if (con->peer_id == -1) {
		con->peer_id = peer_id;
		peerId_to_steamId[peer_id] = con;
	} else if (con->peer_id == peer_id) {
		//peer already exists, so nothing happens
	} else {
		// REVIEW Debug messages
		UtilityFunctions::print("Steam ID detected with wrong peer ID: ", (long int)steam_id.to_int());
		UtilityFunctions::print("Peer ID was: ", con->peer_id);
		UtilityFunctions::print("Trying to set as: ", peer_id);
	}
}

void SteamMultiplayerPeer::set_listen_socket(const int listen_socket) {
	this->listen_socket = listen_socket;
}

int SteamMultiplayerPeer::get_listen_socket() const {
	return listen_socket;
}

Dictionary SteamMultiplayerPeer::get_peer_map() {
	Dictionary output;
	for (HashMap<int64_t, Ref<SteamConnection>>::ConstIterator E = connections_by_steamId64.begin(); E; ++E) {
		output[E->value->peer_id] = E->value->steam_id.to_int();
	}
	return output;
}