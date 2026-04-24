#include "protocol.h"
#include "session.h"
#include "utils.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

static void test_fail(const char *test_name, const char *fmt, ...)
{
	va_list args;

	fprintf(stderr, "[FAIL] %s: ", test_name);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	failures++;
}

#define ASSERT_TRUE(test_name, expr) \
	do { \
		if (!(expr)) { \
			test_fail(test_name, "assertion failed: %s", #expr); \
			return; \
		} \
	} while (0)

#define ASSERT_EQ_INT(test_name, actual, expected) \
	do { \
		long long actual_ = (long long)(actual); \
		long long expected_ = (long long)(expected); \
		if (actual_ != expected_) { \
			test_fail(test_name, \
				  "expected %s == %s, got %lld vs %lld", \
				  #actual, \
				  #expected, \
				  actual_, \
				  expected_); \
			return; \
		} \
	} while (0)

#define ASSERT_EQ_STR(test_name, actual, expected) \
	do { \
		if (strcmp((actual), (expected)) != 0) { \
			test_fail(test_name, \
				  "expected \"%s\", got \"%s\"", \
				  (expected), \
				  (actual)); \
			return; \
		} \
	} while (0)

static bool str_is_lower_hex(const char *str)
{
	size_t i;

	for (i = 0; str[i]; i++) {
		if (!isdigit((unsigned char)str[i]) &&
		    !(str[i] >= 'a' && str[i] <= 'f'))
			return false;
	}

	return true;
}

static void reset_session_globals(void)
{
	size_t i;

	for (i = 0; i < num_old_sessions; i++) {
		free(old_session_ids[i]);
		old_session_ids[i] = NULL;
	}

	num_old_sessions = 0;
	memset(tcp_sessions, 0, sizeof(tcp_sessions));
}

static void test_sized_str_eq_exact_match(void)
{
	const char *test_name = __func__;

	ASSERT_TRUE(test_name, sized_str_eq("hello", "hello", 5));
}

static void test_sized_str_eq_rejects_different_length(void)
{
	const char *test_name = __func__;

	ASSERT_TRUE(test_name, !sized_str_eq("hello", "hello!", 6));
	ASSERT_TRUE(test_name, !sized_str_eq("hello", "hell", 4));
}

static void test_gen_random_id_returns_lower_hex_string(void)
{
	const char *test_name = __func__;
	char *random_id = gen_random_id(12);

	ASSERT_TRUE(test_name, random_id != NULL);
	ASSERT_EQ_INT(test_name, strlen(random_id), 12);
	ASSERT_TRUE(test_name, str_is_lower_hex(random_id));
	free(random_id);
}

static void test_gen_client_id_formats_expected_prefix(void)
{
	const char *test_name = __func__;
	char client_id[64];

	memset(client_id, 0, sizeof(client_id));
	gen_client_id(client_id, sizeof(client_id));

	ASSERT_TRUE(test_name, strncmp(client_id, "mqtt-forward-", 13) == 0);
	ASSERT_EQ_INT(test_name, strlen(client_id), 25);
	ASSERT_TRUE(test_name, str_is_lower_hex(client_id + 13));
}

static void test_gen_session_id_returns_32_nibbles(void)
{
	const char *test_name = __func__;
	char *session_id = gen_session_id();

	ASSERT_TRUE(test_name, session_id != NULL);
	ASSERT_EQ_INT(test_name, strlen(session_id), 32);
	ASSERT_TRUE(test_name, str_is_lower_hex(session_id));
	free(session_id);
}

static void test_create_config_header_empty_config(void)
{
	const char *test_name = __func__;
	struct tcp_session_config session_cfg = {0};
	struct tcp_over_mqtt_remote_config_hdr *remote_cfg;
	uint8_t *cfg_hdr_buf = NULL;
	size_t cfg_hdr_size = 0;
	int ret;

	ret = create_config_header(&session_cfg, &cfg_hdr_buf, &cfg_hdr_size);

	ASSERT_EQ_INT(test_name, ret, 0);
	ASSERT_TRUE(test_name, cfg_hdr_buf != NULL);
	ASSERT_EQ_INT(test_name,
		      cfg_hdr_size,
		      sizeof(struct tcp_over_mqtt_remote_config_hdr));

	remote_cfg = (struct tcp_over_mqtt_remote_config_hdr *)cfg_hdr_buf;
	ASSERT_EQ_INT(test_name, remote_cfg->config_size, cfg_hdr_size);
	ASSERT_EQ_INT(test_name, remote_cfg->num_config_items, 0);
	free(cfg_hdr_buf);
}

static void test_create_config_header_with_ip_and_port(void)
{
	const char *test_name = __func__;
	struct tcp_session_config session_cfg = {
		.ip_addr = inet_addr("192.0.2.42"),
		.port = 1883,
	};
	struct tcp_over_mqtt_remote_config_hdr *remote_cfg;
	struct tcp_over_mqtt_remote_config_item_hdr *item_hdr;
	struct tcp_over_mqtt_remote_config_ip_addr *item_ip_addr;
	struct tcp_over_mqtt_remote_config_port *item_port;
	uint8_t *cfg_hdr_buf = NULL;
	size_t cfg_hdr_size = 0;
	size_t expected_size;
	int ret;

	expected_size = sizeof(struct tcp_over_mqtt_remote_config_hdr) +
			2 * sizeof(struct tcp_over_mqtt_remote_config_item_hdr) +
			sizeof(struct tcp_over_mqtt_remote_config_ip_addr) +
			sizeof(struct tcp_over_mqtt_remote_config_port);

	ret = create_config_header(&session_cfg, &cfg_hdr_buf, &cfg_hdr_size);

	ASSERT_EQ_INT(test_name, ret, 0);
	ASSERT_TRUE(test_name, cfg_hdr_buf != NULL);
	ASSERT_EQ_INT(test_name, cfg_hdr_size, expected_size);

	remote_cfg = (struct tcp_over_mqtt_remote_config_hdr *)cfg_hdr_buf;
	ASSERT_EQ_INT(test_name, remote_cfg->config_size, expected_size);
	ASSERT_EQ_INT(test_name, remote_cfg->num_config_items, 2);

	item_hdr = (struct tcp_over_mqtt_remote_config_item_hdr *)remote_cfg->data;
	ASSERT_EQ_INT(test_name, item_hdr->config_type, REMOTE_CONFIG_TYPE_IP_ADDR);
	item_ip_addr = (struct tcp_over_mqtt_remote_config_ip_addr *)item_hdr->data;
	ASSERT_EQ_INT(test_name, item_ip_addr->ip_addr, session_cfg.ip_addr);

	item_hdr = (struct tcp_over_mqtt_remote_config_item_hdr *)
		   ((uint8_t *)item_hdr +
		    sizeof(*item_hdr) + sizeof(*item_ip_addr));
	ASSERT_EQ_INT(test_name, item_hdr->config_type, REMOTE_CONFIG_TYPE_PORT);
	item_port = (struct tcp_over_mqtt_remote_config_port *)item_hdr->data;
	ASSERT_EQ_INT(test_name, item_port->port, session_cfg.port);
	free(cfg_hdr_buf);
}

static void test_clear_rx_packet_backlog_clears_entries(void)
{
	const char *test_name = __func__;
	struct rx_packet_backlog backlog = {0};

	backlog.backlog[0].buf = calloc(4, 1);
	backlog.backlog[0].len = 4;
	backlog.backlog[7].buf = calloc(2, 1);
	backlog.backlog[7].len = 2;

	ASSERT_TRUE(test_name, backlog.backlog[0].buf != NULL);
	ASSERT_TRUE(test_name, backlog.backlog[7].buf != NULL);

	clear_rx_packet_backlog(&backlog);

	ASSERT_TRUE(test_name, backlog.backlog[0].buf == NULL);
	ASSERT_EQ_INT(test_name, backlog.backlog[0].len, 0);
	ASSERT_TRUE(test_name, backlog.backlog[7].buf == NULL);
	ASSERT_EQ_INT(test_name, backlog.backlog[7].len, 0);
}

static void test_clear_tx_packet_backlog_clears_entries(void)
{
	const char *test_name = __func__;
	struct tx_packet_backlog backlog = {0};

	backlog.backlog[0].buf = calloc(3, 1);
	backlog.backlog[0].len = 3;
	backlog.backlog[8].buf = calloc(5, 1);
	backlog.backlog[8].len = 5;

	ASSERT_TRUE(test_name, backlog.backlog[0].buf != NULL);
	ASSERT_TRUE(test_name, backlog.backlog[8].buf != NULL);

	clear_tx_packet_backlog(&backlog);

	ASSERT_TRUE(test_name, backlog.backlog[0].buf == NULL);
	ASSERT_EQ_INT(test_name, backlog.backlog[0].len, 0);
	ASSERT_TRUE(test_name, backlog.backlog[8].buf == NULL);
	ASSERT_EQ_INT(test_name, backlog.backlog[8].len, 0);
}

static void test_request_session_close_marks_session_closing(void)
{
	const char *test_name = __func__;
	int sockets[2];
	struct tcp_session session = {
		.session_id = "session-1",
		.sock = -1,
	};

	ASSERT_EQ_INT(test_name, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
	session.sock = sockets[0];

	request_session_close(&session);

	ASSERT_TRUE(test_name, session.closing);
	close(sockets[0]);
	close(sockets[1]);
}

static void test_clear_session_releases_resources_and_tracks_old_id(void)
{
	const char *test_name = __func__;
	int sockets[2];
	struct tcp_session session = {0};

	reset_session_globals();

	ASSERT_EQ_INT(test_name, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

	session.session_id = strdup("session-2");
	session.publish_topic = strdup("topic");
	session.rx_buf = calloc(SESSION_RX_BUF_SIZE, 1);
	session.session_cfg = calloc(1, sizeof(*session.session_cfg));
	session.rx_backlog.backlog[0].buf = calloc(1, 1);
	session.rx_backlog.backlog[0].len = 1;
	session.tx_backlog.backlog[0].buf = calloc(1, 1);
	session.tx_backlog.backlog[0].len = 1;
	session.sock = sockets[0];

	ASSERT_TRUE(test_name, session.session_id != NULL);
	ASSERT_TRUE(test_name, session.publish_topic != NULL);
	ASSERT_TRUE(test_name, session.rx_buf != NULL);
	ASSERT_TRUE(test_name, session.session_cfg != NULL);

	clear_session(&session);

	ASSERT_TRUE(test_name, session.session_id == NULL);
	ASSERT_TRUE(test_name, session.publish_topic == NULL);
	ASSERT_TRUE(test_name, session.rx_buf == NULL);
	ASSERT_TRUE(test_name, session.session_cfg == NULL);
	ASSERT_EQ_INT(test_name, session.sock, 0);
	ASSERT_EQ_INT(test_name, num_old_sessions, 1);
	ASSERT_EQ_STR(test_name, old_session_ids[0], "session-2");

	free(old_session_ids[0]);
	old_session_ids[0] = NULL;
	num_old_sessions = 0;
	close(sockets[1]);
}

struct test_case {
	const char *name;
	void (*fn)(void);
};

int main(void)
{
	size_t i;
	const struct test_case tests[] = {
		{"sized_str_eq exact match", test_sized_str_eq_exact_match},
		{"sized_str_eq different length", test_sized_str_eq_rejects_different_length},
		{"gen_random_id lower hex", test_gen_random_id_returns_lower_hex_string},
		{"gen_client_id format", test_gen_client_id_formats_expected_prefix},
		{"gen_session_id length", test_gen_session_id_returns_32_nibbles},
		{"create_config_header empty", test_create_config_header_empty_config},
		{"create_config_header ip and port", test_create_config_header_with_ip_and_port},
		{"clear_rx_packet_backlog", test_clear_rx_packet_backlog_clears_entries},
		{"clear_tx_packet_backlog", test_clear_tx_packet_backlog_clears_entries},
		{"request_session_close", test_request_session_close_marks_session_closing},
		{"clear_session", test_clear_session_releases_resources_and_tracks_old_id},
	};

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		int failed_before = failures;

		tests[i].fn();
		if (failures == failed_before)
			fprintf(stderr, "[PASS] %s\n", tests[i].name);
	}

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "All %zu tests passed\n", sizeof(tests) / sizeof(tests[0]));
	return EXIT_SUCCESS;
}
