/* packet-woww.c
 * Routines for World of Warcraft World dissection
 * Copyright 2021, Gtker <woww@gtker.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The protocol is used for World of Warcraft World packets.
 * These are seen when a client is connected to a world server and plays the game.
 * The WOW protocol (no extra W) packets are Login packets, and they are handled in
 * the packet-wow.c file.
 *
 * More info on world packets and login packets:
 * https://wowdev.wiki/World_Packet
 * https://wowdev.wiki/Login_Packet
 *
 * All World packets contain a header with:
 * * A 16 bit big endian size field.
 * * A (32 or 16 bit) little endian opcode field.
 * Server to client opcodes are 16 bits while client to server opcodes are 32 bits.
 *
 * All world packets other than SMSG_AUTH_CHALLENGE and CMSG_AUTH_SESSION have
 * "encrypted" headers based on a 40 byte session key, however it is relatively
 * easily broken.
 *
 * SMSG packets are Server messages (from server) and CMSG packets are Client messages
 * (from client). MSG packets can be either.
 */

#include <config.h>
#include <epan/packet.h>   /* Should be first Wireshark include (other than config.h) */
#include <epan/conversation.h>   /* Should be first Wireshark include (other than config.h) */
#include <epan/wmem/wmem.h>   /* Should be first Wireshark include (other than config.h) */

/* Prototypes */
void proto_reg_handoff_woww(void);
void proto_register_woww(void);

/* Initialize the protocol and registered fields */
static int proto_woww = -1;

/* Fields that all packets have */
static int hf_woww_size_field = -1;
static int hf_woww_opcode_field = -1;

#define WOWW_TCP_PORT 8085

#define WOWW_CLIENT_TO_SERVER pinfo->destport == WOWW_TCP_PORT
#define WOWW_SERVER_TO_CLIENT pinfo->srcport  == WOWW_TCP_PORT

#define WOWW_HEADER_ARRAY_ALLOC_SIZE 8
#define WOWW_SESSION_KEY_LENGTH 40

static gint ett_woww = -1;

/* Packets that do not have at least a u16 size field and a u16 opcode field are not valid. */
#define WOWW_MIN_LENGTH 4

typedef struct WowwParticipant {
    guint8 last_encrypted_value;
    guint8 index;
    gboolean unencrypted_packet_encountered;
} WowwParticipant_t;

typedef struct WowwConversation {
    guint8 session_key[WOWW_SESSION_KEY_LENGTH];
    wmem_tree_t* decrypted_headers;
    WowwParticipant_t client;
    WowwParticipant_t server;
} WowwConversation_t;

typedef enum {
    SMSG_AUTH_CHALLENGE = 0x1EC,
    CMSG_AUTH_SESSION = 0x1ED,
} world_packets;

static const value_string world_packet_strings[] = {
    { SMSG_AUTH_CHALLENGE, "SMSG_AUTH_CHALLENGE"},
    { CMSG_AUTH_SESSION, "CMSG_AUTH_SESSION"},
    { 0, NULL }
};

static guint8*
get_decrypted_header(const guint8 session_key[WOWW_SESSION_KEY_LENGTH], WowwParticipant_t* participant, const guint8* header, guint8 header_size) {
    guint8* decrypted_header = wmem_alloc0(wmem_file_scope(), 8);

    for (guint8 i = 0; i < header_size; i++) {

        decrypted_header[i] = (header[i] - participant->last_encrypted_value) ^ session_key[participant->index];

        participant->last_encrypted_value = header[i];
        participant->index = (participant->index + 1) % WOWW_SESSION_KEY_LENGTH;
    }

    return decrypted_header;
}

static int
dissect_woww(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
        void *data _U_)
{
    /* Set up structures needed to add the protocol subtree and manage it */
    proto_item *ti;
    proto_tree *woww_tree;
    /* Other misc. local variables. */
    gint offset = 0;
    gint len = 0;
    guint32 opcode = 0;

    /*** HEURISTICS ***/

    /* Check that the packet is long enough for it to belong to us. */
    if (tvb_reported_length(tvb) < WOWW_MIN_LENGTH)
        return 0;

    /* Check that there's enough data present to run the heuristics. If there
     * isn't, reject the packet; it will probably be dissected as data and if
     * the user wants it dissected despite it being short they can use the
     * "Decode-As" functionality. If your heuristic needs to look very deep into
     * the packet you may not want to require *all* data to be present, but you
     * should ensure that the heuristic does not access beyond the captured
     * length of the packet regardless. */
    if (tvb_captured_length(tvb) < 1)
        return 0;

    /*** COLUMN DATA ***/

    /* Set the Protocol column to the constant string of WOWW */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "WOWW");

    /* If you will be fetching any data from the packet before filling in
     * the Info column, clear that column first in case the calls to fetch
     * data from the packet throw an exception so that the Info column doesn't
     * contain data left over from the previous dissector: */
    col_clear(pinfo->cinfo, COL_INFO);

    /*** PROTOCOL TREE ***/

    /* create display subtree for the protocol */
    ti = proto_tree_add_item(tree, proto_woww, tvb, 0, -1, ENC_NA);

    woww_tree = proto_item_add_subtree(ti, ett_woww);

    // Get conversation data
    conversation_t* conv = find_or_create_conversation(pinfo);
    WowwConversation_t* wowwConversation = (WowwConversation_t *)conversation_get_proto_data(conv,
                                                                                             proto_woww);
    if (wowwConversation == NULL) {
        // Assume that file scope means for the lifetime of the dissection
        wowwConversation = (WowwConversation_t*) wmem_new0(wmem_file_scope(), WowwConversation_t);
        conversation_add_proto_data(conv, proto_woww, wowwConversation);
        wowwConversation->decrypted_headers = wmem_tree_new(wmem_file_scope());

        guint8 session_key[WOWW_SESSION_KEY_LENGTH] = {0xfc, 0x97, 0x81, 0xd9, 0xde, 0xbb, 0xc5, 0x8b, 0x84, 0x57, 0x9a, 0xe3, 0xe3, 0xaa, 0x8c, 0x0c, 0x6b, 0xdc, 0x9c, 0xbc, 0x9f, 0x4b, 0x09, 0x35, 0xc5, 0xa9, 0x67, 0xb6, 0x1d, 0x0c, 0x8a, 0xca, 0x56, 0x73, 0xdc, 0xe5, 0x14, 0xcf, 0xd9, 0xdd};
        memcpy(wowwConversation->session_key, session_key, WOWW_SESSION_KEY_LENGTH);
    }

    // Isolate session key for packet
    WowwParticipant_t* participant;
    guint8 headerSize = 4;

    if (WOWW_SERVER_TO_CLIENT) {
        participant = &wowwConversation->server;
        headerSize = 4;
    } else {
        participant = &wowwConversation->client;
        headerSize = 6;
    }

    guint8* decrypted_header = NULL;

    // First time we see this header, we need to decrypt it
    if (!pinfo->fd->visited) {
        guint8* header = wmem_alloc0(wmem_packet_scope(), WOWW_HEADER_ARRAY_ALLOC_SIZE);

        // Read directly
        for (int i = 0; i < headerSize; i++) {
            header[i] = tvb_get_guint8(tvb, offset);
            offset += 1;
        }

        // Only first packet is unencrypted, all the rest are encrypted
        if (participant->unencrypted_packet_encountered) {
            // Do analysis
            decrypted_header = get_decrypted_header(wowwConversation->session_key, participant, header, headerSize);
        } else {
            participant->unencrypted_packet_encountered = true;

            decrypted_header = wmem_alloc0(wmem_file_scope(), WOWW_HEADER_ARRAY_ALLOC_SIZE);
            memcpy(decrypted_header, header, headerSize);
        }

        // Add to hashmap
        wmem_tree_insert32(wowwConversation->decrypted_headers, pinfo->num, decrypted_header);
    }
    // Seen header before, no need to analyze again
    else {
        decrypted_header = wmem_tree_lookup32(wowwConversation->decrypted_headers, pinfo->num);
    }

    // Add to tree
    tvbuff_t *next_tvb = tvb_new_child_real_data(tvb, decrypted_header, headerSize, headerSize);
    add_new_data_source(pinfo, next_tvb, "Decrypted Header");

    /* Add an item to the subtree, see section 1.5 of README.dissector for more
     * information. */
    // We're indexing into another tvb
    offset = 0;
    len = 2;
    proto_tree_add_item(woww_tree, hf_woww_size_field, next_tvb,
            offset, len, ENC_BIG_ENDIAN);
    offset += len;

    if (WOWW_SERVER_TO_CLIENT) {
        len = 2;
        opcode = tvb_get_guint16(next_tvb, offset, ENC_LITTLE_ENDIAN);
    } else if (WOWW_CLIENT_TO_SERVER) {
        len = 4;
        opcode = tvb_get_guint32(next_tvb, offset, ENC_LITTLE_ENDIAN);
    }

    proto_tree_add_item(woww_tree, hf_woww_opcode_field, next_tvb,
                        offset, len, ENC_LITTLE_ENDIAN);

    col_set_str(pinfo->cinfo, COL_INFO, val_to_str_const(opcode,
                                                         world_packet_strings,
                                                         "Encrypted Header"));

    // Remember to go back to original tvb

    return tvb_captured_length(tvb);
}

/* Register the protocol with Wireshark.
 *
 * This format is required because a script is used to build the C function that
 * calls all the protocol registration.
 */
void
proto_register_woww(void)
{
    /* Setup list of header fields  See Section 1.5 of README.dissector for
     * details. */
    static hf_register_info hf[] = {
        { &hf_woww_size_field,
          { "Size", "woww.size",
            FT_UINT16, BASE_HEX_DEC, NULL, 0,
            "Size of the packet including opcode field but not including size field", HFILL }
        },
	{ &hf_woww_opcode_field,
	  { "Opcode", "woww.opcode",
	    FT_UINT32, BASE_HEX, VALS(world_packet_strings), 0,
	    "Opcode of the packet", HFILL }
	}
    };

	/* Setup protocol subtree array */
    static gint *ett[] = {
        &ett_woww
    };

    /* Register the protocol name and description */
    proto_woww = proto_register_protocol("World of Warcraft World",
            "WOWW", "woww");

    /* Required function calls to register the header fields and subtrees */
    proto_register_field_array(proto_woww, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    prefs_register_protocol(proto_woww,
            NULL);

}



/* Simpler form of proto_reg_handoff_woww which can be used if there are
 * no prefs-dependent registration function calls. */
void
proto_reg_handoff_woww(void)
{
    dissector_handle_t woww_handle;

    /* Use create_dissector_handle() to indicate that dissect_woww()
     * returns the number of bytes it dissected (or 0 if it thinks the packet
     * does not belong to World of Warcraft World).
     */
    woww_handle = create_dissector_handle(dissect_woww, proto_woww);
    dissector_add_uint_with_preference("tcp.port", WOWW_TCP_PORT, woww_handle);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */