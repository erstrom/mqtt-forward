#ifndef _SESSION_H_
#define _SESSION_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <mosquitto.h>

#define SESSION_BACKLOG_SIZE 200
#define MAX_SESSIONS 100
#define MAX_LIFETIME_SESSIONS (MAX_SESSIONS*100)
#define SESSION_RX_BUF_SIZE 10000
#define MQTT_TOPIC_MAX_LEN 250

/**
 * Types
 */
struct tcp_session_config {
	uint32_t ip_addr;
	uint16_t port;
};

struct packet_backlog_data {
	uint8_t *buf;
	size_t len;
};

struct rx_packet_backlog {
	struct packet_backlog_data backlog[SESSION_BACKLOG_SIZE];
	uint64_t expected_seq_nbr;
	int read_idx;
};

struct tx_packet_backlog {
	struct packet_backlog_data backlog[SESSION_BACKLOG_SIZE];
	uint64_t acked_seq_nbr;
	int first_unacked_idx;
};

struct tcp_session {
	bool server_session;
	char *session_id;
	char *publish_topic;
	int sock;
	uint8_t *rx_buf;
	uint64_t tx_seq_nbr;
	struct rx_packet_backlog rx_backlog;
	struct tx_packet_backlog tx_backlog;
	struct tcp_session_config *session_cfg;
};

/**
 * Functions
 */
void clear_rx_packet_backlog(struct rx_packet_backlog *rx_backlog);
void clear_tx_packet_backlog(struct tx_packet_backlog *tx_backlog);
void clear_session(struct tcp_session *session_data);
int create_session(const char *session_id,
		   size_t session_id_len,
		   size_t *session_nbr,
		   int tcp_sock,
		   struct sockaddr_in *tcp_server_addr,
		   bool server_session,
		   struct tcp_session_config *session_cfg,
		   struct mosquitto *mqtt_client,
		   int mqtt_qos,
		   char *mqtt_topic_prefix,
		   char *server_mqtt_id,
		   void *(*thread_fn)(void *));

/**
 * External variables
 */
extern struct tcp_session tcp_sessions[MAX_SESSIONS];
extern pthread_mutex_t session_mtx;
extern char *old_session_ids[MAX_LIFETIME_SESSIONS];
extern size_t num_old_sessions;

#endif
