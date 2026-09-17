#include "framework/framework.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../toxcore/tox_struct.h"
#include "../../toxcore/Messenger.h"
#include "../../toxcore/network.h"

#define CODEWORD "RONALD MCDONALD"
#define CODEWORD_LEN (sizeof(CODEWORD) - 1)
#define RELAY_TCP_PORT 33811
#define NUM_TOXES 100

typedef struct {
    uint8_t chat_id[TOX_GROUP_CHAT_ID_SIZE];
    uint32_t group_number;
    uint32_t peer_id;
    bool joined;
    bool got_private_message;
    bool got_group_message;
} State;

static void on_group_peer_join(const Tox_Event_Group_Peer_Join *event, void *user_data)
{
    ToxNode *self = (ToxNode *)user_data;
    State *state = (State *)tox_node_get_script_ctx(self);
    state->peer_id = tox_event_group_peer_join_get_peer_id(event);
    state->joined = true;
    tox_node_log(self, "Peer %u joined group", state->peer_id);
}

static void on_group_private_message(const Tox_Event_Group_Private_Message *event, void *user_data)
{
    ToxNode *self = (ToxNode *)user_data;
    State *state = (State *)tox_node_get_script_ctx(self);
    const uint8_t *message = tox_event_group_private_message_get_message(event);
    const size_t length = tox_event_group_private_message_get_message_length(event);

    if (length == CODEWORD_LEN && memcmp(CODEWORD, message, length) == 0) {
        state->got_private_message = true;
    }
}

static void on_group_message(const Tox_Event_Group_Message *event, void *user_data)
{
    ToxNode *self = (ToxNode *)user_data;
    State *state = (State *)tox_node_get_script_ctx(self);
    const uint8_t *message = tox_event_group_message_get_message(event);
    const size_t length = tox_event_group_message_get_message_length(event);

    if (length == CODEWORD_LEN && memcmp(CODEWORD, message, length) == 0) {
        state->got_group_message = true;
    }
}

static void alice_script(ToxNode *self, void *ctx)
{
    State *state = (State *)ctx;
    Tox *tox = tox_node_get_tox(self);

    tox_events_callback_group_peer_join(tox_node_get_dispatch(self), on_group_peer_join);

    Tox_Err_Group_New new_err;
    state->group_number = tox_group_new(tox, TOX_GROUP_PRIVACY_STATE_PUBLIC, (const uint8_t *)"test", 4,
                                        (const uint8_t *)"test", 4, &new_err);
    ck_assert(new_err == TOX_ERR_GROUP_NEW_OK);

    Tox_Err_Group_State_Query id_err;
    tox_group_get_chat_id(tox, state->group_number, state->chat_id, &id_err);
    ck_assert(id_err == TOX_ERR_GROUP_STATE_QUERY_OK);

    char chat_id_str[TOX_GROUP_CHAT_ID_SIZE * 2 + 1];
    for (int i = 0; i < TOX_GROUP_CHAT_ID_SIZE; ++i) {
        snprintf(chat_id_str + i * 2, sizeof(chat_id_str) - i * 2, "%02X", state->chat_id[i]);
    }
    tox_node_log(self, "Created group with chat_id: %s", chat_id_str);

    tox_scenario_barrier_wait(self); // Barrier 1: chat_id is ready for Bob

    tox_node_log(self, "Waiting for Bob to join group...");
    WAIT_UNTIL(state->joined);
    //tox_scenario_wait_for_event(self, TOX_EVENT_GROUP_PEER_JOIN);
    if (!state->joined) {
        return;
    }

    Tox_Err_Group_Send_Private_Message perr;
    tox_group_send_private_message(tox, state->group_number, state->peer_id,
                                   TOX_MESSAGE_TYPE_NORMAL,
                                   (const uint8_t *)CODEWORD, CODEWORD_LEN, &perr);
    ck_assert(perr == TOX_ERR_GROUP_SEND_PRIVATE_MESSAGE_OK);

    tox_scenario_barrier_wait(self); // Barrier 2: PM sent
}

static void bob_script(ToxNode *self, void *ctx)
{
    State *state = (State *)ctx;
    Tox *tox = tox_node_get_tox(self);

    tox_events_callback_group_private_message(tox_node_get_dispatch(self), on_group_private_message);
    tox_events_callback_group_message(tox_node_get_dispatch(self), on_group_message);

    tox_node_log(self, "Waiting for TCP connection...");
    WAIT_UNTIL(tox_node_get_connection_status(self) == TOX_CONNECTION_TCP);
	uint64_t ts_connected = tox_scenario_get_time(tox_node_get_scenario(self));

	// wait for 31min
    while (tox_scenario_get_time(tox_node_get_scenario(self)) < (31*60*1000+ts_connected) && tox_scenario_is_running(self)) {
        tox_scenario_yield(self);
    }

    tox_scenario_barrier_wait(self); // Barrier 1: Alice has chat_id

    ToxScenario *s = tox_node_get_scenario(self);
    ToxNode *alice_node = tox_scenario_get_node(s, 0);
    State *alice_state = (State *)tox_node_get_script_ctx(alice_node);

    tox_node_log(self, "Joining group via chat_id...");
    Tox_Err_Group_Join jerr;
    state->group_number = tox_group_join(tox, alice_state->chat_id, (const uint8_t *)"test", 4, nullptr, 0, &jerr);
    ck_assert_uint_eq(jerr, TOX_ERR_GROUP_JOIN_OK);

    WAIT_UNTIL(state->got_private_message);
    if (!state->got_private_message) {
        return;
    }
    tox_scenario_barrier_wait(self); // Barrier 2: PM received, now leaving
}

static void relay_script(ToxNode *self, void *ctx)
{
    (void)ctx;
    tox_scenario_barrier_wait(self); // Barrier 1
    tox_scenario_barrier_wait(self); // Barrier 2
}

static void peer_script(ToxNode *self, void *ctx)
{
   (void)ctx;
    //tox_node_wait_for_self_connected(self);
    tox_scenario_barrier_wait(self); // Barrier 1
    tox_scenario_barrier_wait(self); // Barrier 2
}

int main(int argc, char **argv)
{
    ToxScenario *s = tox_scenario_new(argc, argv, 40 * 60 * 1000);

    struct Tox_Options *options_alice = tox_options_new(nullptr);
    tox_options_set_udp_enabled(options_alice, false);

    struct Tox_Options *options_bob = tox_options_new(nullptr);
    tox_options_set_udp_enabled(options_bob, false);

    struct Tox_Options *options_relay0 = tox_options_new(nullptr);
    tox_options_set_tcp_port(options_relay0, RELAY_TCP_PORT);

    struct Tox_Options *options_relay1 = tox_options_new(nullptr);
    tox_options_set_tcp_port(options_relay1, RELAY_TCP_PORT+1);

    struct Tox_Options *options_relay2 = tox_options_new(nullptr);
    tox_options_set_tcp_port(options_relay2, RELAY_TCP_PORT+2);

    State *alice_state = (State *)calloc(1, sizeof(State));
    State *bob_state = (State *)calloc(1, sizeof(State));

    ToxNode *alice = tox_scenario_add_node_ex(s, "Alice", alice_script, alice_state, sizeof(State), options_alice);
    ToxNode *bob = tox_scenario_add_node_ex(s, "Bob", bob_script, bob_state, sizeof(State), options_bob);
    ToxNode *relay0 = tox_scenario_add_node_ex(s, "Relay0", relay_script, nullptr, 0, options_relay0);
    ToxNode *relay1 = tox_scenario_add_node_ex(s, "Relay1", relay_script, nullptr, 0, options_relay1);
    ToxNode *relay2 = tox_scenario_add_node_ex(s, "Relay2", relay_script, nullptr, 0, options_relay2);


    tox_options_free(options_alice);
    tox_options_free(options_bob);
    tox_options_free(options_relay0);
    tox_options_free(options_relay1);
    tox_options_free(options_relay2);

    if (!alice || !bob || !relay0 || !relay1 || !relay2) {
        fprintf(stderr, "Failed to create base nodes\n");
        return 1;
    }

	/*networking_registerhandler(tox_node_get_tox(relay0)->m->net, NET_PACKET_ANNOUNCE_REQUEST, nullptr, nullptr);*/
	/*networking_registerhandler(tox_node_get_tox(relay0)->m->net, NET_PACKET_ANNOUNCE_RESPONSE, nullptr, nullptr);*/
	/*networking_registerhandler(tox_node_get_tox(relay1)->m->net, NET_PACKET_ANNOUNCE_REQUEST, nullptr, nullptr);*/
	/*networking_registerhandler(tox_node_get_tox(relay1)->m->net, NET_PACKET_ANNOUNCE_RESPONSE, nullptr, nullptr);*/
	/*networking_registerhandler(tox_node_get_tox(relay2)->m->net, NET_PACKET_ANNOUNCE_REQUEST, nullptr, nullptr);*/
	/*networking_registerhandler(tox_node_get_tox(relay2)->m->net, NET_PACKET_ANNOUNCE_RESPONSE, nullptr, nullptr);*/

    uint8_t relay0_dht_id[TOX_PUBLIC_KEY_SIZE];
    tox_self_get_dht_id(tox_node_get_tox(relay0), relay0_dht_id);
    uint8_t relay1_dht_id[TOX_PUBLIC_KEY_SIZE];
    tox_self_get_dht_id(tox_node_get_tox(relay1), relay1_dht_id);
    uint8_t relay2_dht_id[TOX_PUBLIC_KEY_SIZE];
    tox_self_get_dht_id(tox_node_get_tox(relay2), relay2_dht_id);

    uint16_t relay0_tcp_port = tox_self_get_tcp_port(tox_node_get_tox(relay0), nullptr);
    uint16_t relay1_tcp_port = tox_self_get_tcp_port(tox_node_get_tox(relay1), nullptr);
    uint16_t relay2_tcp_port = tox_self_get_tcp_port(tox_node_get_tox(relay2), nullptr);

    tox_node_bootstrap(relay0, relay1);
    tox_node_bootstrap(relay0, relay2);

    Tox_Err_Bootstrap berr;

#if 1
    struct Tox_Options *opts_peer = tox_options_new(nullptr);
    tox_options_set_udp_enabled(opts_peer, true);
    tox_options_set_local_discovery_enabled(opts_peer, false);

    State states[NUM_TOXES] = {0};
    ToxNode *nodes[NUM_TOXES];
    for (int i = 0; i < NUM_TOXES; ++i) {
        char alias[16];
        snprintf(alias, sizeof(alias), "Tox%d", i);
        nodes[i] = tox_scenario_add_node_ex(s, alias, peer_script, &states[i], sizeof(State), opts_peer);

//		if (i % 2 == 0) {
			Tox* t = tox_node_get_tox(nodes[i]);
			networking_registerhandler(t->m->net, NET_PACKET_ANNOUNCE_REQUEST, nullptr, nullptr);
			networking_registerhandler(t->m->net, NET_PACKET_ANNOUNCE_RESPONSE, nullptr, nullptr);
		//}

        // All peers use the Relay for TCP and DHT bootstrapping
        tox_add_tcp_relay(tox_node_get_tox(nodes[i]), "127.0.0.1", relay0_tcp_port, relay0_dht_id, nullptr);
        tox_node_bootstrap(nodes[i], relay0);
        tox_add_tcp_relay(tox_node_get_tox(nodes[i]), "127.0.0.1", relay1_tcp_port, relay1_dht_id, nullptr);
        tox_node_bootstrap(nodes[i], relay1);
        tox_add_tcp_relay(tox_node_get_tox(nodes[i]), "127.0.0.1", relay2_tcp_port, relay2_dht_id, nullptr);
        tox_node_bootstrap(nodes[i], relay2);
    }
    tox_options_free(opts_peer);
#endif

    // Both Alice and Bob use the Relay for TCP and DHT bootstrapping
    //tox_add_tcp_relay(tox_node_get_tox(alice), "127.0.0.1", relay0_tcp_port, relay0_dht_id, &berr);
    tox_add_tcp_relay(tox_node_get_tox(bob), "127.0.0.1", relay0_tcp_port, relay0_dht_id, &berr);
    tox_add_tcp_relay(tox_node_get_tox(alice), "127.0.0.1", relay1_tcp_port, relay1_dht_id, &berr);
    tox_add_tcp_relay(tox_node_get_tox(bob), "127.0.0.1", relay1_tcp_port, relay1_dht_id, &berr);
    tox_add_tcp_relay(tox_node_get_tox(alice), "127.0.0.1", relay2_tcp_port, relay2_dht_id, &berr);
    //tox_add_tcp_relay(tox_node_get_tox(bob), "127.0.0.1", relay2_tcp_port, relay2_dht_id, &berr);

    //tox_node_bootstrap(alice, relay0);
    tox_node_bootstrap(alice, relay1);
    tox_node_bootstrap(alice, relay2);
    tox_node_bootstrap(bob, relay0);
    tox_node_bootstrap(bob, relay1);
    //tox_node_bootstrap(bob, relay2);

    ToxScenarioStatus res = tox_scenario_run(s);
    tox_scenario_free(s);
    free(alice_state);
    free(bob_state);

	if (res == TOX_SCENARIO_DONE) {
    	fprintf(stderr, "\nSUCCESS\n");
	} else {
    	fprintf(stderr, "\nFAILURE\n");
	}

    return (res == TOX_SCENARIO_DONE) ? 0 : 1;
}

#undef RELAY_TCP_PORT
