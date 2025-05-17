#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <arpa/inet.h>
#include "mysock.h"
#include "stcp_api.h"
#include "transport.h"

typedef enum {
    CSTATE_LISTEN = 0,
    CSTATE_SYN_SENT,
    CSTATE_SYN_RECEIVED,
    CSTATE_ESTABLISHED,
    CSTATE_FIN_WAIT_1,
    CSTATE_FIN_WAIT_2,
    CSTATE_CLOSING,
    CSTATE_TIME_WAIT,
    CSTATE_CLOSE_WAIT,
    CSTATE_LAST_ACK
} connection_state_t;

#define RECV_BUF_SIZE 3072
#define SEND_BUF_SIZE 3072

typedef struct {
    bool_t done;
    connection_state_t connection_state;
    tcp_seq initial_sequence_num;
    tcp_seq next_seq_num;
    tcp_seq expected_seq_num;
} context_t;

static void generate_initial_seq_num(context_t *ctx);
static void control_loop(mysocket_t sd, context_t *ctx);

void transport_init(mysocket_t sd, bool_t is_active) {
    context_t *ctx = (context_t *) calloc(1, sizeof(context_t));
    assert(ctx);
    generate_initial_seq_num(ctx);

    STCPHeader header;
    memset(&header, 0, sizeof(STCPHeader));
    header.th_off = 5;
    header.th_win = htons(RECV_BUF_SIZE);

    if (is_active) {
        header.th_flags = TH_SYN;
        header.th_seq = htonl(ctx->initial_sequence_num);
        stcp_network_send(sd, &header, sizeof(header), NULL);

        uint8_t buf[sizeof(STCPHeader)];
        stcp_network_recv(sd, buf, sizeof(buf));
        STCPHeader *recv_hdr = (STCPHeader *) buf;

        if ((recv_hdr->th_flags & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK)) {
            ctx->expected_seq_num = ntohl(recv_hdr->th_seq) + 1;
            ctx->next_seq_num = ctx->initial_sequence_num + 1;

            memset(&header, 0, sizeof(STCPHeader));
            header.th_off = 5;
            header.th_flags = TH_ACK;
            header.th_seq = htonl(ctx->next_seq_num);
            header.th_ack = htonl(ctx->expected_seq_num);
            header.th_win = htons(RECV_BUF_SIZE);
            stcp_network_send(sd, &header, sizeof(header), NULL);

            ctx->connection_state = CSTATE_ESTABLISHED;
        }
    } else {
        uint8_t buf[sizeof(STCPHeader)];
        stcp_network_recv(sd, buf, sizeof(buf));
        STCPHeader *recv_hdr = (STCPHeader *) buf;

        if (recv_hdr->th_flags & TH_SYN) {
            ctx->expected_seq_num = ntohl(recv_hdr->th_seq) + 1;
            ctx->next_seq_num = ctx->initial_sequence_num;

            memset(&header, 0, sizeof(STCPHeader));
            header.th_off = 5;
            header.th_flags = TH_SYN | TH_ACK;
            header.th_seq = htonl(ctx->initial_sequence_num);
            header.th_ack = htonl(ctx->expected_seq_num);
            header.th_win = htons(RECV_BUF_SIZE);
            stcp_network_send(sd, &header, sizeof(header), NULL);

            stcp_network_recv(sd, buf, sizeof(buf));
            recv_hdr = (STCPHeader *) buf;

            if (recv_hdr->th_flags & TH_ACK) {
                ctx->next_seq_num = ctx->initial_sequence_num + 1;
                ctx->connection_state = CSTATE_ESTABLISHED;
            }
        }
    }

    stcp_unblock_application(sd);
    control_loop(sd, ctx);
    free(ctx);
}

static void generate_initial_seq_num(context_t *ctx) {
    assert(ctx);
    ctx->initial_sequence_num = 1;
}

static void control_loop(mysocket_t sd, context_t *ctx) {
    uint8_t packet[STCP_MSS + sizeof(STCPHeader)];
    uint8_t payload[STCP_MSS];

    while (!ctx->done) {
        unsigned int event = stcp_wait_for_event(sd, APP_DATA | NETWORK_DATA | APP_CLOSE_REQUESTED, NULL);

        if (event & APP_DATA) {
            size_t len = stcp_app_recv(sd, payload, STCP_MSS);

            STCPHeader header;
            memset(&header, 0, sizeof(header));
            header.th_off = 5;
            header.th_flags = TH_ACK;
            header.th_seq = htonl(ctx->next_seq_num);
            header.th_ack = htonl(ctx->expected_seq_num);
            header.th_win = htons(RECV_BUF_SIZE);

            stcp_network_send(sd, &header, sizeof(header), payload, len, NULL);
            ctx->next_seq_num += len;
        }

        if (event & NETWORK_DATA) {
            ssize_t rcv_len = stcp_network_recv(sd, packet, sizeof(packet));
            STCPHeader *hdr = (STCPHeader *) packet;
            uint32_t seq = ntohl(hdr->th_seq);
            uint16_t flags = hdr->th_flags;
            size_t header_len = hdr->th_off * 4;
            size_t data_len = rcv_len - header_len;
            uint8_t *data = packet + header_len;

            if (flags & TH_FIN) {
                ctx->expected_seq_num = seq + 1;
                STCPHeader finack;
                memset(&finack, 0, sizeof(finack));
                finack.th_off = 5;
                finack.th_flags = TH_ACK;
                finack.th_seq = htonl(ctx->next_seq_num);
                finack.th_ack = htonl(ctx->expected_seq_num);
                finack.th_win = htons(RECV_BUF_SIZE);
                stcp_network_send(sd, &finack, sizeof(finack), NULL);
                stcp_fin_received(sd);
            } else if (seq == ctx->expected_seq_num && data_len > 0) {
                stcp_app_send(sd, data, data_len);
                ctx->expected_seq_num += data_len;

                STCPHeader ack;
                memset(&ack, 0, sizeof(ack));
                ack.th_off = 5;
                ack.th_flags = TH_ACK;
                ack.th_seq = htonl(ctx->next_seq_num);
                ack.th_ack = htonl(ctx->expected_seq_num);
                ack.th_win = htons(RECV_BUF_SIZE);
                stcp_network_send(sd, &ack, sizeof(ack), NULL);
            }
        }

        if (event & APP_CLOSE_REQUESTED) {
            STCPHeader fin;
            memset(&fin, 0, sizeof(fin));
            fin.th_off = 5;
            fin.th_flags = TH_FIN;
            fin.th_seq = htonl(ctx->next_seq_num);
            fin.th_ack = htonl(ctx->expected_seq_num);
            fin.th_win = htons(RECV_BUF_SIZE);
            stcp_network_send(sd, &fin, sizeof(fin), NULL);
            ctx->next_seq_num++;

            ssize_t rcv_len = stcp_network_recv(sd, packet, sizeof(packet));
            STCPHeader *hdr = (STCPHeader *) packet;
            if (hdr->th_flags & TH_ACK) {
                ctx->done = TRUE;
            }
        }
    }
}