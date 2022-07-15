/**
 * Copyright (c) 2022 Brian Starkey <stark3y@gmail.com>
 *
 * Based on the Pico W tcp_server example:
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

extern const char *wifi_ssid;
extern const char *wifi_pass;

#define TCP_PORT 4242
#define DEBUG_printf printf
#define BUF_SIZE 2048
#define POLL_TIME_S 5

typedef struct TCP_SERVER_T_ {
	struct tcp_pcb *server_pcb;
	struct tcp_pcb *client_pcb;
	bool complete;
	uint8_t buffer_recv[BUF_SIZE];
	int sent_len;
} TCP_SERVER_T;

static TCP_SERVER_T* tcp_server_init(void) {
	TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
	if (!state) {
		DEBUG_printf("failed to allocate state\n");
		return NULL;
	}
	return state;
}

static err_t tcp_server_close(void *arg) {
	TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
	err_t err = ERR_OK;
	if (state->client_pcb != NULL) {
		tcp_arg(state->client_pcb, NULL);
		tcp_poll(state->client_pcb, NULL, 0);
		tcp_sent(state->client_pcb, NULL);
		tcp_recv(state->client_pcb, NULL);
		tcp_err(state->client_pcb, NULL);
		err = tcp_close(state->client_pcb);
		if (err != ERR_OK) {
			DEBUG_printf("close failed %d, calling abort\n", err);
			tcp_abort(state->client_pcb);
			err = ERR_ABRT;
		}
		state->client_pcb = NULL;
	}
	if (state->server_pcb) {
		tcp_arg(state->server_pcb, NULL);
		tcp_close(state->server_pcb);
		state->server_pcb = NULL;
	}
	return err;
}

static err_t tcp_server_result(void *arg, int status) {
	TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
	if (status == 0) {
		DEBUG_printf("completed normally\n");
	} else {
		DEBUG_printf("error %d\n", status);
	}
	state->complete = true;
	return tcp_server_close(arg);
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
	TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
	DEBUG_printf("tcp_server_sent %u\n", len);
	state->sent_len += len;

	if (state->sent_len >= strlen("hello\n")) {
		DEBUG_printf("Sending done\n");
	}

	return ERR_OK;
}

err_t tcp_server_send_data(void *arg, struct tcp_pcb *tpcb)
{
	TCP_SERVER_T *state = (TCP_SERVER_T*)arg;

	state->sent_len = 0;
	DEBUG_printf("Writing to client\n");
	// this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
	// can use this method to cause an assertion in debug mode, if this method is called when
	// cyw43_arch_lwip_begin IS needed
	cyw43_arch_lwip_check();
	err_t err = tcp_write(tpcb, "hello\n", strlen("hello\n"), TCP_WRITE_FLAG_COPY);
	if (err != ERR_OK) {
		DEBUG_printf("Failed to write data %d\n", err);
		return tcp_server_result(arg, -1);
	}
	return ERR_OK;
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
	TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
	if (!p) {
		return tcp_server_result(arg, 0);
	}
	// this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
	// can use this method to cause an assertion in debug mode, if this method is called when
	// cyw43_arch_lwip_begin IS needed
	cyw43_arch_lwip_check();
	if (p->tot_len > 0) {
		DEBUG_printf("tcp_server_recv %d err %d\n", p->tot_len, err);

		// Receive the buffer
		pbuf_copy_partial(p, state->buffer_recv, p->tot_len, 0);

		state->buffer_recv[p->tot_len] = '\0';
		printf("%s\n", state->buffer_recv);
		tcp_recved(tpcb, p->tot_len);
	}
	pbuf_free(p);

	return ERR_OK;
}

static err_t tcp_server_poll(void *arg, struct tcp_pcb *tpcb) {
	DEBUG_printf("tcp_server_poll_fn\n");
	return ERR_OK;
}

static void tcp_server_err(void *arg, err_t err) {
	if (err != ERR_ABRT) {
		DEBUG_printf("tcp_client_err_fn %d\n", err);
		tcp_server_result(arg, err);
	} else {
		DEBUG_printf("tcp_client_err_fn abort %d\n", err);
		tcp_server_result(arg, err);
	}
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
	TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
	if (err != ERR_OK || client_pcb == NULL) {
		DEBUG_printf("Failure in accept\n");
		tcp_server_result(arg, err);
		return ERR_VAL;
	}
	DEBUG_printf("Client connected\n");

	state->client_pcb = client_pcb;
	tcp_arg(client_pcb, state);
	tcp_sent(client_pcb, tcp_server_sent);
	tcp_recv(client_pcb, tcp_server_recv);
	tcp_poll(client_pcb, tcp_server_poll, POLL_TIME_S * 2);
	tcp_err(client_pcb, tcp_server_err);

	return tcp_server_send_data(arg, state->client_pcb);
}

static bool tcp_server_open(void *arg) {
	TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
	DEBUG_printf("Starting server at %s on port %u\n", ip4addr_ntoa(netif_ip4_addr(netif_list)), TCP_PORT);

	struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
	if (!pcb) {
		DEBUG_printf("failed to create pcb\n");
		return false;
	}

	err_t err = tcp_bind(pcb, NULL, TCP_PORT);
	if (err) {
		DEBUG_printf("failed to bind to port %d\n");
		return false;
	}

	state->server_pcb = tcp_listen_with_backlog(pcb, 1);
	if (!state->server_pcb) {
		DEBUG_printf("failed to listen\n");
		if (pcb) {
			tcp_close(pcb);
		}
		return false;
	}

	tcp_arg(state->server_pcb, state);
	tcp_accept(state->server_pcb, tcp_server_accept);

	return true;
}

void run_tcp_server(void) {
	TCP_SERVER_T *state = tcp_server_init();
	if (!state) {
		return;
	}

	if (!tcp_server_open(state)) {
		tcp_server_result(state, -1);
		return;
	}

	// Block until the connection is closed
	while(!state->complete) {
		// the following #ifdef is only here so this same example can be used in multiple modes;
		// you do not need it in your code
#if PICO_CYW43_ARCH_POLL
		// if you are using pico_cyw43_arch_poll, then you must poll periodically from your
		// main loop (not from a timer) to check for WiFi driver or lwIP work that needs to be done.
		cyw43_arch_poll();
		sleep_ms(1);
#else
		// if you are not using pico_cyw43_arch_poll, then WiFI driver and lwIP work
		// is done via interrupt in the background. This sleep is just an example of some (blocking)
		// work you might be doing.
		sleep_ms(1000);
#endif
	}
	free(state);
}

int main() {
	stdio_init_all();

	sleep_ms(1000);

	if (cyw43_arch_init()) {
		printf("failed to initialise\n");
		return 1;
	}

	cyw43_arch_enable_sta_mode();

	printf("Connecting to WiFi...\n");
	if (cyw43_arch_wifi_connect_timeout_ms(wifi_ssid, wifi_pass, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
		printf("failed to connect.\n");
		return 1;
	} else {
		printf("Connected.\n");
	}

	for ( ; ; ) {
		run_tcp_server();
	}

	cyw43_arch_deinit();
	return 0;
}