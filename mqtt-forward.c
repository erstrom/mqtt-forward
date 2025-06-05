#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <mosquitto.h>
#include <getopt.h>
#include <poll.h>
#include <openssl/rand.h>

#define MAX_SESSIONS 100
#define MAX_LIFETIME_SESSIONS (MAX_SESSIONS*100)
#define SESSION_RX_BUF_SIZE 10000
#define SESSION_BACKLOG_SIZE 200
#define TX_WINDOW_LIMIT 100
#define MAX_SERVER_LIST 30
#define MQTT_TOPIC_MAX_LEN 250

#define DBG_LOG_(fmt, ...) \
	do { \
		if (debug) \
			printf(fmt, __VA_ARGS__); \
	} while (0)

#define ENV_VAR_MQTT_HOST   "MQTT_FORWARD_MQTT_HOST"
#define ENV_VAR_ROOT_CA     "MQTT_FORWARD_ROOT_CA"
#define ENV_VAR_CERTIFICATE "MQTT_FORWARD_CERTIFICATE"
#define ENV_VAR_PRIVATE_KEY "MQTT_FORWARD_PRIVATE_KEY"
#define ENV_VAR_TOPIC_PREFIX "MQTT_FORWARD_TOPIC_PREFIX"
#define ENV_VAR_REMOTE_IP   "MQTT_FORWARD_REMOTE_IP"
#define ENV_VAR_REMOTE_PORT "MQTT_FORWARD_REMOTE_PORT"

/**
 * Internal types
 */

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

struct tcp_session_config {
	uint32_t ip_addr;
	uint16_t port;
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

struct tcp_server_list {
	char *server_id;
	time_t last_seen_time;
};

/**
 * Message types (all structs are packed)
 */
struct tcp_over_mqtt_hdr {
	uint64_t seq_nbr;
	uint64_t acked_seq_nbr;
#define TCP_OVER_MQTT_FLAG_TCP_DISCONNECT (1)
#define TCP_OVER_MQTT_FLAG_ACKED_SEQ_NBR (2)
#define TCP_OVER_MQTT_FLAG_NO_DATA (4)
#define TCP_OVER_MQTT_FLAG_BEACON (8)
#define TCP_OVER_MQTT_FLAG_REMOTE_CONFIG (16)
	uint32_t flags;
	uint32_t pad;
} __attribute__((packed));

struct tcp_over_mqtt_remote_config_hdr {
	/**
	 * Total size (in bytes) of the config message
	 */
	uint32_t config_size;
	/**
	 * Number of config items in the message
	 */
	uint32_t num_config_items;
	uint8_t data[];
} __attribute__((packed));

struct tcp_over_mqtt_remote_config_item_hdr {
	/**
	 * config_type has any of the values defined in
	 * enum tcp_over_mqtt_remote_config_type
	 */
	uint16_t config_type;
	uint8_t data[];
} __attribute__((packed));

struct tcp_over_mqtt_remote_config_ip_addr {
	uint32_t ip_addr;
} __attribute__((packed));

struct tcp_over_mqtt_remote_config_port {
	uint16_t port;
} __attribute__((packed));

enum tcp_over_mqtt_remote_config_type {
	REMOTE_CONFIG_TYPE_IP_ADDR,
	REMOTE_CONFIG_TYPE_PORT,
};

/**
 * Global variables
 */
static int tcp_client_listen_sock;
static int tcp_client_listen_port;
static int tcp_server_connect_port;
static pthread_t tcp_accept_thread;
static pthread_t beacon_print_thread;
static pthread_t beacon_tx_thread;
static pthread_t mqtt_create_thread;
static pthread_t tcp_rx_threads[MAX_SESSIONS];
static struct mosquitto *g_mqtt_client;
static bool connected_to_mqtt_server;
static bool server_mode;
static bool list_servers;
static bool transmit_beacons;
static struct tcp_session tcp_sessions[MAX_SESSIONS];
static struct sockaddr_in *tcp_server_addr;
static char *old_session_ids[MAX_LIFETIME_SESSIONS];
static size_t num_old_sessions;
static bool debug;
static struct tcp_server_list server_list[MAX_SESSIONS];

static pthread_mutex_t session_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t server_list_mtx = PTHREAD_MUTEX_INITIALIZER;

static bool use_tls;
static bool remote_tcp_port_set;
static bool remote_tcp_server_addr_set;
static uint32_t remote_tcp_server_addr;
static uint16_t remote_tcp_port;
static char server_mqtt_id[100];
static char client_mqtt_id[100];
static int mqtt_port;
static int mqtt_qos = 1;
static char mqtt_host[100];
static char mqtt_root_ca[100];
static char mqtt_certificate[100];
static char mqtt_private_key[100];
static char mqtt_topic_prefix[50];
static char tcp_server_addr_str[100];

static void clear_rx_packet_backlog(struct rx_packet_backlog *rx_backlog)
{
	int i;

	for (i = 0; i < SESSION_BACKLOG_SIZE; i++)
		free(rx_backlog->backlog[i].buf);

	memset(&rx_backlog->backlog[0],
	       0,
	       sizeof(rx_backlog->backlog[0])*SESSION_BACKLOG_SIZE);
}

static void clear_tx_packet_backlog(struct tx_packet_backlog *tx_backlog)
{
	int i;

	for (i = 0; i < SESSION_BACKLOG_SIZE; i++)
		free(tx_backlog->backlog[i].buf);

	memset(&tx_backlog->backlog[0],
	       0,
	       sizeof(tx_backlog->backlog[0])*SESSION_BACKLOG_SIZE);
}

static void clear_session(struct tcp_session *session_data)
{
	fprintf(stderr, "%s: TCP session %s terminated\n",
		__func__, session_data->session_id);
	pthread_mutex_lock(&session_mtx);
	/* Store the session ID of the cleared session in the old session id
	 * array
	 */
	old_session_ids[num_old_sessions++] = session_data->session_id;
	free(session_data->rx_buf);
	free(session_data->publish_topic);
	free(session_data->session_cfg);
	clear_rx_packet_backlog(&session_data->rx_backlog);
	clear_tx_packet_backlog(&session_data->tx_backlog);
	memset(session_data, 0, sizeof(*session_data));
	pthread_mutex_unlock(&session_mtx);
}

static char *gen_random_id(size_t n_nibbles)
{
	int i;
	uint8_t *id;
	uint8_t nibble_1st;
	uint8_t nibble_2nd;
	char *random_id;

	random_id = calloc(n_nibbles+1, 1);
	id = calloc(n_nibbles/2, 1);

	RAND_bytes(id, n_nibbles/2);

	for (i = 0; i < n_nibbles/2; i++) {
		nibble_1st = (id[i] & 0xf0) >> 4;
		nibble_2nd = id[i] & 0x0f;
		nibble_1st = (nibble_1st > 9) ?
			(nibble_1st - 10 + 'a') : (nibble_1st + '0');
		nibble_2nd = (nibble_2nd > 9) ?
			(nibble_2nd - 10 + 'a') : (nibble_2nd + '0');
		random_id[2*i] = nibble_1st;
		random_id[2*i+1] = nibble_2nd;
	}

	free(id);

	return random_id;
}

static void gen_client_id(void)
{
	char *random_id;

	random_id = gen_random_id(12);

	snprintf(client_mqtt_id,
		 sizeof(client_mqtt_id),
		 "mqtt-forward-%s",
		 random_id);
	free(random_id);
}

static char *gen_session_id()
{
	return gen_random_id(32);
}

static int create_config_header(const struct tcp_session_config *session_cfg,
				uint8_t **cfg_hdr_buf,
				size_t *cfg_hdr_size)
{
	uint8_t *hdr_buf;
	struct tcp_over_mqtt_remote_config_hdr *remote_cfg;
	struct tcp_over_mqtt_remote_config_item_hdr *item_hdr;
	struct tcp_over_mqtt_remote_config_ip_addr *item_ip_addr;
	struct tcp_over_mqtt_remote_config_port *item_port;
	char *tmp_ptr;
	size_t hdr_size;

	/* Calculate size of config header */
	hdr_size = sizeof(struct tcp_over_mqtt_remote_config_hdr);
	hdr_size += session_cfg->ip_addr ?
		(sizeof(struct tcp_over_mqtt_remote_config_item_hdr) +
		 sizeof(struct tcp_over_mqtt_remote_config_ip_addr)) : 0;
	hdr_size += session_cfg->port ?
		(sizeof(struct tcp_over_mqtt_remote_config_item_hdr) +
		 sizeof(struct tcp_over_mqtt_remote_config_port)) : 0;

	/* Allocate and populate config header */
	hdr_buf = calloc(hdr_size, 1);
	if (!hdr_buf) {
		fprintf(stderr, "%s: Unable to allocate header buf\n",
			__func__);
		return -1;
	}

	remote_cfg = (struct tcp_over_mqtt_remote_config_hdr *)hdr_buf;
	remote_cfg->config_size = hdr_size;
	item_hdr = (struct tcp_over_mqtt_remote_config_item_hdr *)remote_cfg->data;
	tmp_ptr = (char *)item_hdr;
	if (session_cfg->ip_addr) {
		item_hdr->config_type = REMOTE_CONFIG_TYPE_IP_ADDR;
		item_ip_addr =
			(struct tcp_over_mqtt_remote_config_ip_addr *)item_hdr->data;
		item_ip_addr->ip_addr = session_cfg->ip_addr;
		tmp_ptr += sizeof(*item_hdr) + sizeof(*item_ip_addr);
		remote_cfg->num_config_items++;
	}

	item_hdr = (struct tcp_over_mqtt_remote_config_item_hdr *)tmp_ptr;
	if (session_cfg->port) {
		item_hdr->config_type = REMOTE_CONFIG_TYPE_PORT;
		item_port =
			(struct tcp_over_mqtt_remote_config_port *)item_hdr->data;
		item_port->port = session_cfg->port;
		tmp_ptr += sizeof(*item_hdr) + sizeof(*item_ip_addr);
		remote_cfg->num_config_items++;
	}

	*cfg_hdr_buf = hdr_buf;
	*cfg_hdr_size = hdr_size;

	return 0;
}

static void *tcp_session_rx_thread_fn(void *arg)
{
	uint8_t *rx_buf;
	uint8_t *cfg_hdr_buf;
	size_t cfg_hdr_size;
	int ret;
	int recv_len;
	struct tcp_session *session_data;
	struct tcp_over_mqtt_hdr tx_hdr;
	int sock;
	int backlog_offset;
	int backlog_write_idx;
	char *topic;
	struct tx_packet_backlog *tx_backlog;
	struct rx_packet_backlog *rx_backlog;
	struct timespec sleep_time = {.tv_nsec = 100000000};
	struct pollfd fds = {.events = POLLIN};

	session_data = (struct tcp_session *)arg;

	rx_buf = session_data->rx_buf;
	sock = session_data->sock;
	fds.fd = sock;
	topic = session_data->publish_topic;
	tx_backlog = &session_data->tx_backlog;
	rx_backlog = &session_data->rx_backlog;
	cfg_hdr_size = 0;

	if (!session_data->server_session && session_data->session_cfg) {
		/* Create the config header that will be used in the first
		 * outgoing message (for clients only)
		 */
		ret = create_config_header(session_data->session_cfg,
					   &cfg_hdr_buf,
					   &cfg_hdr_size);
		if (ret < 0)
			goto out;
	}

	for (;;) {
		pthread_mutex_lock(&session_mtx);
		backlog_offset = session_data->tx_seq_nbr - tx_backlog->acked_seq_nbr - 1;

		if (backlog_offset > TX_WINDOW_LIMIT) {
			/* TX window is full. Stop reading more data from input
			 * socket and start re-transmit the lost frame
			 */
			struct tcp_over_mqtt_hdr *retransmit_tx_hdr =
				(struct tcp_over_mqtt_hdr *)tx_backlog->backlog[tx_backlog->first_unacked_idx].buf;

			if (rx_backlog->expected_seq_nbr > 1) {
				retransmit_tx_hdr->flags |= TCP_OVER_MQTT_FLAG_ACKED_SEQ_NBR;
				retransmit_tx_hdr->acked_seq_nbr =
					rx_backlog->expected_seq_nbr - 1;
			} else {
				retransmit_tx_hdr->flags = 0;
			}

			DBG_LOG_("Session %s: TX RETRANSMIT: %4lu. Acked %4lu\n",
				 session_data->session_id,
				 retransmit_tx_hdr->seq_nbr,
				 retransmit_tx_hdr->acked_seq_nbr);

			ret = mosquitto_publish(g_mqtt_client,
						NULL,
						topic,
						tx_backlog->backlog[tx_backlog->first_unacked_idx].len,
						tx_backlog->backlog[tx_backlog->first_unacked_idx].buf,
						mqtt_qos,
						false /*retain*/);
			pthread_mutex_unlock(&session_mtx);
			(void)nanosleep(&sleep_time, NULL);
		} else {
			/* Only read from TCP socket if the TX window is not exceeded */
			pthread_mutex_unlock(&session_mtx);
			ret = poll(&fds, 1, 500);
			if (ret < 0) {
				fprintf(stderr, "%s: poll errno: %d\n", __func__, errno);
				break;
			} else if (ret == 0) {
				/* Timeout */
				tx_hdr.seq_nbr = 0;
				tx_hdr.flags = TCP_OVER_MQTT_FLAG_NO_DATA;
				if (rx_backlog->expected_seq_nbr > 1) {
					tx_hdr.flags |= TCP_OVER_MQTT_FLAG_ACKED_SEQ_NBR;
					tx_hdr.acked_seq_nbr =
						rx_backlog->expected_seq_nbr - 1;
				}
				tx_hdr.flags |= cfg_hdr_size ?
					TCP_OVER_MQTT_FLAG_REMOTE_CONFIG : 0;
				memcpy(rx_buf, &tx_hdr, sizeof(tx_hdr));
				recv_len = sizeof(tx_hdr);
				if (cfg_hdr_size) {
					memcpy(rx_buf + sizeof(tx_hdr), cfg_hdr_buf, cfg_hdr_size);
					recv_len += cfg_hdr_size;
					/* The config header is only used in the first
					 * message
					 */
					free(cfg_hdr_buf);
					cfg_hdr_buf = NULL;
					cfg_hdr_size = 0;
				}
				DBG_LOG_("Session %s: TX HEARTBEAT: %4lu. Acked %4lu\n",
					 session_data->session_id,
					 tx_hdr.seq_nbr,
					 tx_hdr.acked_seq_nbr);
				ret = mosquitto_publish(g_mqtt_client,
							NULL,
							topic,
							recv_len,
							rx_buf,
							mqtt_qos,
							false /*retain*/);
				if (ret) {

					fprintf(stderr, "Publishing on topic %s failed. Result %d\n",
						topic,
						ret);
					break;
				}
				continue;
			}

			/* Data has been received on socket */
			recv_len = recv(sock,
					rx_buf + sizeof(tx_hdr) + cfg_hdr_size,
					SESSION_RX_BUF_SIZE - sizeof(tx_hdr) - cfg_hdr_size,
					0);
			if (recv_len < 0) {
				fprintf(stderr, "%s: recv errno: %d\n", __func__, errno);
				break;
			} else if (recv_len == 0) {
				fprintf(stderr, "%s: TCP connection terminated\n", __func__);
				break;
			}

			pthread_mutex_lock(&session_mtx);
			tx_hdr.seq_nbr = session_data->tx_seq_nbr;
			tx_hdr.flags = 0;
			if (rx_backlog->expected_seq_nbr > 1) {
				tx_hdr.flags |= TCP_OVER_MQTT_FLAG_ACKED_SEQ_NBR;
				tx_hdr.acked_seq_nbr = rx_backlog->expected_seq_nbr - 1;
			}
			tx_hdr.flags |= cfg_hdr_size ?
				TCP_OVER_MQTT_FLAG_REMOTE_CONFIG : 0;
			memcpy(rx_buf, &tx_hdr, sizeof(tx_hdr));
			recv_len += sizeof(tx_hdr);
			if (cfg_hdr_size) {
				memcpy(rx_buf + sizeof(tx_hdr), cfg_hdr_buf, cfg_hdr_size);
				recv_len += cfg_hdr_size;
				/* The config header is only used in the first
				 * message
				 */
				free(cfg_hdr_buf);
				cfg_hdr_buf = NULL;
				cfg_hdr_size = 0;
			}

			/* re-read backlog_offset since acked_seq_nbr might have
			 * been update during the sleep of this thread.
			 */
			backlog_offset = session_data->tx_seq_nbr - tx_backlog->acked_seq_nbr - 1;
			backlog_write_idx =
				(tx_backlog->first_unacked_idx + backlog_offset) % SESSION_BACKLOG_SIZE;
			if (tx_backlog->backlog[backlog_write_idx].buf) {
				fprintf(stderr, "Session: %s: Backlog index %d already has an allocated buffer!\n",
					session_data->session_id,
					backlog_write_idx);
				free(tx_backlog->backlog[backlog_write_idx].buf);
			}
			tx_backlog->backlog[backlog_write_idx].buf = malloc(recv_len);
			tx_backlog->backlog[backlog_write_idx].len = recv_len;
			memcpy(tx_backlog->backlog[backlog_write_idx].buf, rx_buf, recv_len);
			session_data->tx_seq_nbr++;
			DBG_LOG_("Session %s: TX: %4lu. Acked %4lu\n",
				 session_data->session_id,
				 tx_hdr.seq_nbr,
				 tx_hdr.acked_seq_nbr);
			pthread_mutex_unlock(&session_mtx);
			ret = mosquitto_publish(g_mqtt_client,
						NULL,
						topic,
						recv_len,
						rx_buf,
						mqtt_qos,
						false /*retain*/);
			if (ret) {

				fprintf(stderr, "Publishing on topic %s failed. Result %d\n",
					topic,
					ret);
				break;
			}
		}
	}
out:
	fprintf(stderr, "%s: TCP session ended\n", __func__);

	clear_session(session_data);

	return NULL;
}

static int connect_server_session(const struct tcp_session_config *session_cfg,
				  int *tcp_sock)
{
	int ret;
	static struct sockaddr_in server_addr;

	server_addr = *tcp_server_addr;

	/* Create TCP socket and connect to the server*/
	*tcp_sock = socket(AF_INET, SOCK_STREAM, 0);

	if (session_cfg) {
		if (session_cfg->ip_addr)
			server_addr.sin_addr.s_addr = session_cfg->ip_addr;
		if (session_cfg->port)
			server_addr.sin_port = htons(session_cfg->port);
	}

	ret = connect(*tcp_sock,
		      (const struct sockaddr *) &server_addr,
		      sizeof(server_addr));
	if (ret) {
		fprintf(stderr, "%s: Unable to establish TCP connection. errno %d\n",
			__func__, errno);
		return -1;
	}

	return 0;
}

static int create_session(const char *session_id,
			  size_t session_id_len,
			  size_t *session_nbr,
			  int tcp_sock,
			  bool server_session,
			  struct tcp_session_config *session_cfg)
{
	int ret;
	size_t session_nbr_local;
	char *session_id_local;

	for (session_nbr_local = 0;
	     session_nbr_local < MAX_SESSIONS;
	     session_nbr_local++) {
		if (!tcp_sessions[session_nbr_local].session_id)
			break;
	}

	if (session_nbr_local >= MAX_SESSIONS) {
		fprintf(stderr, "%s: No empty session slot found for new session\n",
			 __func__);
		return -1;
	}

	/* Server sessions are created from a received session ID.
	 * Clients will create their own ID
	 */
	if (server_session) {
		session_id_local = calloc(session_id_len + 1, 1);
		strncpy(session_id_local,
			session_id,
			session_id_len);

	} else {
		session_id_local = gen_session_id();
	}

	if (server_session) {
		ret = connect_server_session(session_cfg, &tcp_sock);
		if (ret)
			return -1;
	} else {
		/* Subscribe to data from the server for this session_id */
		int mid;
		char topic[MQTT_TOPIC_MAX_LEN];

		snprintf(topic,
			 sizeof(topic),
			 "%s/%s/%s/rx",
			 mqtt_topic_prefix,
			 server_mqtt_id,
			 session_id_local);

		ret = mosquitto_subscribe(g_mqtt_client,
					  &mid,
					  topic,
					  mqtt_qos);
		if (ret) {
			fprintf(stderr, "%s: mosquitto_subscribe %d (failed to subscribe to topic %s)\n",
				__func__, ret, topic);
			return -1;

		}
		fprintf(stderr, "%s: subscribed to %s\n", __func__, topic);
	}

	/* Create a session struct for the session*/
	memset(&tcp_sessions[session_nbr_local], 0, sizeof(tcp_sessions[0]));
	tcp_sessions[session_nbr_local].server_session = server_session;
	tcp_sessions[session_nbr_local].session_id = session_id_local;
	tcp_sessions[session_nbr_local].sock = tcp_sock;
	tcp_sessions[session_nbr_local].rx_buf = calloc(SESSION_RX_BUF_SIZE, 1);
	tcp_sessions[session_nbr_local].rx_backlog.expected_seq_nbr = 1;
	tcp_sessions[session_nbr_local].tx_seq_nbr = 1;
	tcp_sessions[session_nbr_local].publish_topic = calloc(MQTT_TOPIC_MAX_LEN, 1);
	tcp_sessions[session_nbr_local].session_cfg = session_cfg;
	if (server_session)
		snprintf(tcp_sessions[session_nbr_local].publish_topic,
			 MQTT_TOPIC_MAX_LEN,
			 "%s/%s/%s/rx",
			 mqtt_topic_prefix,
			 server_mqtt_id,
			 session_id_local);
	else
		snprintf(tcp_sessions[session_nbr_local].publish_topic,
			 MQTT_TOPIC_MAX_LEN,
			 "%s/%s/%s/tx",
			 mqtt_topic_prefix,
			 server_mqtt_id,
			 session_id_local);

	fprintf(stderr, "%s: Creating session thread for session %s, session number %lu\n",
		 __func__,
		 tcp_sessions[session_nbr_local].session_id,
		 session_nbr_local);
	ret = pthread_create(&tcp_rx_threads[session_nbr_local],
			     NULL,
			     &tcp_session_rx_thread_fn,
			     &tcp_sessions[session_nbr_local]);
	if (ret) {
		fprintf(stderr, "%s: pthread_create %d\n", __func__, errno);
		return -1;
	}

	*session_nbr = session_nbr_local;

	return 0;
}

static void *tcp_accept_thread_fn(void *arg)
{
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof(client_addr);
	int ret;
	int client_sock;
	size_t session_nbr;
	struct tcp_session_config *session_cfg;

	for (;;) {
		client_sock = accept(tcp_client_listen_sock,
				     (struct sockaddr *)&client_addr,
				     &addr_len);
		fprintf(stderr, "%s: Accepted TCP connection\n", __func__);

		if (client_sock < 0) {
			fprintf(stderr, "connect to remote server failed. errno %d\n", errno);
			close(client_sock);
			continue;
		}

		session_cfg = NULL;
		if (remote_tcp_port_set || remote_tcp_server_addr_set) {
			session_cfg = calloc(sizeof(*session_cfg), 1);
			session_cfg->ip_addr =
				remote_tcp_server_addr_set ? remote_tcp_server_addr : 0;
			session_cfg->port =
				remote_tcp_port_set ? remote_tcp_port : 0;
		}

		pthread_mutex_lock(&session_mtx);
		ret = create_session(NULL, /*client session*/
				     0,
				     &session_nbr,
				     client_sock,
				     false, /*client session*/
				     session_cfg);
		pthread_mutex_unlock(&session_mtx);
		if (ret < 0) {
			fprintf(stderr, "%s: Unable to create client session\n", __func__);
			close(client_sock);
			continue;
		}
	}

	return NULL;
}

static void *create_thread_fn(void *arg)
{
	int ret;
	struct timespec ts = {.tv_sec = 5};

	(void)arg;

	for (;;) {
		if (connected_to_mqtt_server) {
			(void)nanosleep(&ts, NULL);
			continue;
		}

		ret = mosquitto_connect_async(g_mqtt_client,
					      mqtt_host,
					      mqtt_port,
					      20);

		if (ret != MOSQ_ERR_SUCCESS) {
			fprintf(stderr, "%s: mosquitto_connect_async %d\n",
				  __func__, ret);

			/* Try again */
			fprintf(stderr, "%s: Unable to connect to %s. Retrying in %ld sec\n",
				  __func__, mqtt_host, ts.tv_sec);
		}

		(void)nanosleep(&ts, NULL);
	}

	return NULL;
}

static void *beacon_tx_thread_fn(void *arg)
{
	struct timespec ts = {.tv_sec = 3};
	struct tcp_over_mqtt_hdr msg_hdr = {.flags = TCP_OVER_MQTT_FLAG_BEACON};
	char topic[MQTT_TOPIC_MAX_LEN];

	snprintf(topic, sizeof(topic), "%s/%s/beacon/rx", mqtt_topic_prefix, server_mqtt_id);

	for (;;) {
		if (!connected_to_mqtt_server) {
			(void)nanosleep(&ts, NULL);
			continue;
		}

		(void)mosquitto_publish(g_mqtt_client,
					NULL,
					topic,
					sizeof(msg_hdr),
					&msg_hdr,
					mqtt_qos,
					false /*retain*/);
		(void)nanosleep(&ts, NULL);
	}

	return NULL;
}

static void *beacon_print_thread_fn(void *arg)
{
	size_t server_nbr;
	time_t cur_time;
	struct timespec ts = {.tv_sec = 3};

	for (;;) {
		(void)nanosleep(&ts, NULL);

		pthread_mutex_lock(&server_list_mtx);
		cur_time = time(NULL);

		/* Print the server list */
		printf("Detected servers:\n\n");
		printf("  %40s%30s\n\n", "Server ID", "Last seen (seconds ago)");

		for (server_nbr = 0; server_nbr < MAX_SERVER_LIST; server_nbr++) {
			if (!server_list[server_nbr].server_id)
				break;

			printf("  %40s%30ld\n",
			       server_list[server_nbr].server_id,
			       cur_time - server_list[server_nbr].last_seen_time);
		}
		printf("\n\n");
		pthread_mutex_unlock(&server_list_mtx);
	}

	return NULL;
}

static void handle_mqtt_message_list_servers(const char *recvd_client_id,
					     size_t recvd_client_id_len)
{
	size_t server_nbr;
	time_t cur_time;

	pthread_mutex_lock(&server_list_mtx);
	cur_time = time(NULL);

	/* Add the server to the server list */
	for (server_nbr = 0; server_nbr < MAX_SERVER_LIST; server_nbr++) {
		if (!server_list[server_nbr].server_id) {
			server_list[server_nbr].server_id =
				calloc(recvd_client_id_len + 1, 1);
			strncpy(server_list[server_nbr].server_id,
				recvd_client_id,
				recvd_client_id_len);
			server_list[server_nbr].last_seen_time = cur_time;
			break;
		}

		if (strncmp(server_list[server_nbr].server_id,
			    recvd_client_id,
			    recvd_client_id_len) == 0) {
			server_list[server_nbr].last_seen_time = cur_time;
			break;
		}
	}

	pthread_mutex_unlock(&server_list_mtx);

	if (server_nbr >= MAX_SERVER_LIST)
		fprintf(stderr, "Server list exhausted!\n");
}

static void handle_remote_config(struct tcp_over_mqtt_remote_config_hdr *remote_cfg,
				 struct tcp_session_config *session_cfg)
{
	int cfg_idx;
	struct tcp_over_mqtt_remote_config_item_hdr *item_hdr;
	struct tcp_over_mqtt_remote_config_ip_addr *item_ip_addr;
	struct tcp_over_mqtt_remote_config_port *item_port;
	char *tmp_ptr;

	item_hdr = (struct tcp_over_mqtt_remote_config_item_hdr *)remote_cfg->data;
	for (cfg_idx = 0; cfg_idx < remote_cfg->num_config_items; cfg_idx++) {
		tmp_ptr = (char *)item_hdr;
		switch (item_hdr->config_type) {
		case REMOTE_CONFIG_TYPE_IP_ADDR:
			item_ip_addr =
				(struct tcp_over_mqtt_remote_config_ip_addr *)item_hdr->data;
			session_cfg->ip_addr = item_ip_addr->ip_addr;
			tmp_ptr += sizeof(*item_hdr) + sizeof(*item_ip_addr);
			break;
		case REMOTE_CONFIG_TYPE_PORT:
			item_port =
				(struct tcp_over_mqtt_remote_config_port *)item_hdr->data;
			session_cfg->port = item_port->port;
			tmp_ptr += sizeof(*item_hdr) + sizeof(*item_port);
			break;
		default:
			goto out;
		}
		item_hdr = (struct tcp_over_mqtt_remote_config_item_hdr *)tmp_ptr;
	}
out:
	return;
}

static void handle_mqtt_message(uint8_t *msg,
				int msg_len,
				const char *session_id,
				size_t session_id_len,
				const char *recvd_client_id,
				size_t recvd_client_id_len,
				bool server_session)
{
	int ret;
	int i;
	int data_len;
	uint8_t *data;
	int backlog_write_idx;
	int backlog_offset;
	struct tcp_over_mqtt_hdr *rx_hdr;
	struct tcp_over_mqtt_remote_config_hdr *remote_cfg;
	struct rx_packet_backlog *rx_backlog;
	struct tx_packet_backlog *tx_backlog;
	size_t session_nbr;
	size_t remote_cfg_offset = 0;
	struct tcp_session_config *session_cfg = NULL;

	rx_hdr = (struct tcp_over_mqtt_hdr *) msg;

	if (rx_hdr->flags & TCP_OVER_MQTT_FLAG_BEACON) {
		if (list_servers)
			handle_mqtt_message_list_servers(recvd_client_id,
							 recvd_client_id_len);

		return;
	}

	/* Check if there is a session for this session ID */
	for (session_nbr = 0; session_nbr < MAX_SESSIONS; session_nbr++) {
		if ((tcp_sessions[session_nbr].session_id) &&
		    (strncmp(tcp_sessions[session_nbr].session_id, session_id, session_id_len) == 0)) {
			break;
		}
	}

	if (session_nbr >= MAX_SESSIONS) {
		/* No session for this session ID. If we are running as a client
		 * we should discard this message. If running as a server we
		 * should create a new session
		 */
		if (!server_session)
			return;

		/* Before a new session is created we must check if the message
		 * belongs to an old session. In this case it should be
		 * discarded
		 */
		for (i = 0; i < num_old_sessions; i++) {
			if (strncmp(old_session_ids[i], session_id, session_id_len) == 0) {
				fprintf(stderr, "%s: Session ID %s is an old session. Message discarded\n",
					__func__, old_session_ids[i]);
				return;
			}
		}

		if (rx_hdr->flags & TCP_OVER_MQTT_FLAG_REMOTE_CONFIG) {
			remote_cfg = (struct tcp_over_mqtt_remote_config_hdr *)(msg + sizeof(*rx_hdr));
			remote_cfg_offset = remote_cfg->config_size;
			session_cfg = calloc(sizeof(*session_cfg), 1);
			handle_remote_config(remote_cfg, session_cfg);
		}

		pthread_mutex_lock(&session_mtx);
		ret = create_session(session_id,
				     session_id_len,
				     &session_nbr,
				     0,
				     true,  /*server session*/
				     session_cfg);
		pthread_mutex_unlock(&session_mtx);
		if (ret)
			return;
	}

	rx_backlog = &tcp_sessions[session_nbr].rx_backlog;
	tx_backlog = &tcp_sessions[session_nbr].tx_backlog;

	if (rx_hdr->flags & TCP_OVER_MQTT_FLAG_ACKED_SEQ_NBR)
		DBG_LOG_("Session %s: RX: %4lu. Remote acked %4lu\n",
			 tcp_sessions[session_nbr].session_id,
			 rx_hdr->seq_nbr,
			 rx_hdr->acked_seq_nbr);
	else
		DBG_LOG_("Session %s: RX: %4lu. No remote ack\n",
			 tcp_sessions[session_nbr].session_id,
			 rx_hdr->seq_nbr);

	pthread_mutex_lock(&session_mtx);
	if (rx_hdr->flags & TCP_OVER_MQTT_FLAG_ACKED_SEQ_NBR) {
		if (rx_hdr->acked_seq_nbr >= tcp_sessions[session_nbr].tx_seq_nbr) {
			/* The acked sequence number can't equal or exceed the
			 * TX sequence number we are about to send. In this
			 * case, the received frame is some kind of artifact
			 * from a previous session. The session is thus invalid
			 * and should be removed.
			 */
			clear_session(&tcp_sessions[session_nbr]);
			return;
		}

		for (i = tx_backlog->acked_seq_nbr; i < rx_hdr->acked_seq_nbr; i++) {
			if (!tx_backlog->backlog[tx_backlog->first_unacked_idx].buf) {
				fprintf(stderr, "%s: TX backlog index %d already free'd!\n",
					__func__, tx_backlog->first_unacked_idx);
				continue;
			}

			free(tx_backlog->backlog[tx_backlog->first_unacked_idx].buf);
			tx_backlog->backlog[tx_backlog->first_unacked_idx].buf = NULL;
			tx_backlog->backlog[tx_backlog->first_unacked_idx].len = 0xdead;
			tx_backlog->first_unacked_idx++;
			tx_backlog->first_unacked_idx %= SESSION_BACKLOG_SIZE;
		}

		tx_backlog->acked_seq_nbr = (rx_hdr->acked_seq_nbr > tx_backlog->acked_seq_nbr) ?
			rx_hdr->acked_seq_nbr : tx_backlog->acked_seq_nbr;

	}
	pthread_mutex_unlock(&session_mtx);

	if (rx_hdr->flags & TCP_OVER_MQTT_FLAG_NO_DATA) {
		DBG_LOG_("Session %s: No data frame received\n",
			 tcp_sessions[session_nbr].session_id);
		return;
	}

	data_len = msg_len - sizeof(*rx_hdr) - remote_cfg_offset;
	data = msg + sizeof(*rx_hdr) + remote_cfg_offset;
	if (data_len <= 0) {
		/* Invalid data */
		return;
	}

	if (rx_backlog->expected_seq_nbr > rx_hdr->seq_nbr) {
		/* Ignore old packets */
		return;
	}

	backlog_offset = rx_hdr->seq_nbr - rx_backlog->expected_seq_nbr;

	if (backlog_offset < 0) {
		fprintf(stderr, "%s: backlog offset is negative! Corrupt input data?\n", __func__);
		fprintf(stderr, "RX sequence number: %4lu\n",
			rx_hdr->seq_nbr);
		return;
	}

	if (backlog_offset > SESSION_BACKLOG_SIZE - 2) {
		fprintf(stderr, "%s: backlog exceeded\n", __func__);
		/* A packet is probably lost :(
		 * move forward in the backlog until a valid packet is found
		 */
		while (!rx_backlog->backlog[rx_backlog->read_idx].buf) {
			fprintf(stderr, "%s: Packet %ld dropped!\n",
				__func__,
				rx_backlog->expected_seq_nbr);
			rx_backlog->expected_seq_nbr++;
			rx_backlog->read_idx++;
			rx_backlog->read_idx %= SESSION_BACKLOG_SIZE;
			backlog_offset--;
		}
	}

	backlog_write_idx =
		(rx_backlog->read_idx + backlog_offset) % SESSION_BACKLOG_SIZE;

	if (rx_backlog->backlog[backlog_write_idx].buf) {
		fprintf(stderr, "%s: Backlog buffer write index %d taken. Freeing.\n",
			__func__, backlog_write_idx);
		free(rx_backlog->backlog[backlog_write_idx].buf);
	}

	rx_backlog->backlog[backlog_write_idx].buf = malloc(data_len);
	rx_backlog->backlog[backlog_write_idx].len = data_len;
	memcpy(rx_backlog->backlog[backlog_write_idx].buf, data, data_len);

	while (rx_backlog->backlog[rx_backlog->read_idx].buf) {
		ret = send(tcp_sessions[session_nbr].sock,
			   rx_backlog->backlog[rx_backlog->read_idx].buf,
			   rx_backlog->backlog[rx_backlog->read_idx].len,
			   MSG_NOSIGNAL);
		if ((ret < 0) && (errno == EPIPE)) {
			fprintf(stderr, "%s: Remote socket closed. Clearing session %s\n",
				__func__, tcp_sessions[session_nbr].session_id);
			clear_session(&tcp_sessions[session_nbr]);
			return;
		}

		free(rx_backlog->backlog[rx_backlog->read_idx].buf);
		rx_backlog->backlog[rx_backlog->read_idx].buf = NULL;
		rx_backlog->backlog[rx_backlog->read_idx].len = 0xdead;

		rx_backlog->expected_seq_nbr++;
		rx_backlog->read_idx++;
		rx_backlog->read_idx %= SESSION_BACKLOG_SIZE;
	}
}

static void on_message(struct mosquitto *mosq,
		       void *obj,
		       const struct mosquitto_message *message)
{
	char *msg_name;
	char *client_id;
	char *session_id;
	ssize_t client_id_len;
	ssize_t session_id_len;

	(void)mosq;
	(void)obj;

	client_id = strchr(message->topic, '/');
	if (!client_id) {
		fprintf(stderr, "%s: Unable to extract client_id from received topic: %s\n",
			__func__, message->topic);
		return;
	}
	client_id++;

	session_id = strchr(client_id, '/');
	if (!session_id) {
		fprintf(stderr, "%s: Unable to extract session_id from received topic: %s\n",
			__func__, message->topic);
		return;
	}
	session_id++; /* Skip leading '/' */

	client_id_len = session_id - client_id - 1;

	msg_name = strchr(session_id, '/');
	if (!msg_name) {
		fprintf(stderr, "%s: Unable to extract msg_name from received topic: %s\n",
			__func__, message->topic);
		return;
	}
	msg_name++;

	session_id_len = msg_name - session_id - 1;

	msg_name = strrchr(message->topic, '/');
	if (!msg_name) {
		fprintf(stderr, "Invalid message topic received: %s\n", message->topic);
		return;
	}

	msg_name++; /* Skip leading '/' */

	if ((server_mode && (strcmp(msg_name, "tx") == 0)) ||
	    (!server_mode && (strcmp(msg_name, "rx") == 0))) {
		handle_mqtt_message(message->payload,
				    message->payloadlen,
				    session_id,
				    session_id_len,
				    client_id,
				    client_id_len,
				    server_mode);
	} else {
		fprintf(stderr, "%s: %s: unsupported topic: %s\n",
			__func__,
			server_mode ? "SERVER" : "CLIENT",
			message->topic);
	}
}

static void on_subscribe(struct mosquitto *mosq,
			 void *obj,
			 int mid,
			 int qos_count,
			 const int *granted_qos)
{
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
	int ret;
	int mid;
	char topic[MQTT_TOPIC_MAX_LEN];

	(void)mosq;
	(void)obj;

	if (rc) {
		connected_to_mqtt_server = false;
		fprintf(stderr, "%s: Connect error: %d\n",
			 __func__, rc);
		return;
	}

	connected_to_mqtt_server = true;
	fprintf(stderr, "Successfully connected to broker!\n");

	if (server_mode) {
		snprintf(topic,
			 MQTT_TOPIC_MAX_LEN,
			 "%s/%s/+/tx",
			 mqtt_topic_prefix,
			 server_mqtt_id);

		ret = mosquitto_subscribe(g_mqtt_client,
					  &mid,
					  topic,
					  mqtt_qos);
		if (ret) {
			fprintf(stderr, "%s: mosquitto_subscribe %d (failed to subscribe to topic %s)\n",
				__func__, ret, topic);

		}
	} else if (list_servers) {
		snprintf(topic,
			 MQTT_TOPIC_MAX_LEN,
			 "%s/+/beacon/rx",
			 mqtt_topic_prefix);

		ret = mosquitto_subscribe(g_mqtt_client,
					  &mid,
					  topic,
					  mqtt_qos);
		if (ret) {
			fprintf(stderr, "%s: mosquitto_subscribe %d (failed to subscribe to topic %s)\n",
				__func__, ret, topic);

		}
	}
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
	(void)mosq;
	(void)obj;
	connected_to_mqtt_server = false;
	fprintf(stderr, "%s: Disconnected from broker. reason %d\n",
		  __func__, rc);
}

int mqtt_forward_init(void)
{
	int ret;
	int protocol_version = MQTT_PROTOCOL_V311;
	struct sockaddr_in tcp_listen_addr = {
		.sin_family = AF_INET,
		.sin_addr = {.s_addr = INADDR_ANY},   /* internet address */
	};
	struct addrinfo *hostaddrinfo;
	struct addrinfo addrhint = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	char *client_id = server_mode ? server_mqtt_id : client_mqtt_id;

	if (server_mode) {
		ret = getaddrinfo(tcp_server_addr_str,
				  NULL,
				  &addrhint,
				  &hostaddrinfo);
		if (ret) {
			fprintf(stderr, "%s: getaddrinfo returned %d\n", __func__, ret);
			goto err;
		}

		tcp_server_addr = (struct sockaddr_in *)hostaddrinfo->ai_addr;
		tcp_server_addr->sin_port = htons(tcp_server_connect_port);
	} else if (!list_servers) {
		tcp_client_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
		if (tcp_client_listen_sock < 0)
			goto err;

		tcp_listen_addr.sin_port = htons(tcp_client_listen_port);
		ret = bind(tcp_client_listen_sock,
			   (struct sockaddr *) &tcp_listen_addr,
			   sizeof(tcp_listen_addr));
		if (ret) {
			fprintf(stderr, "Unable to bind port %d. bind errno: %d\n",
				tcp_client_listen_port, errno);
			goto err;
		}

		ret = listen(tcp_client_listen_sock, 128);
		if (ret) {
			fprintf(stderr, "TCP listen errno: %d\n", errno);
			goto err;
		}
	}

	ret = mosquitto_lib_init();
	if (ret) {
		fprintf(stderr, "%s: mosquitto_lib_init %d\n", __func__, ret);
		goto err;
	}

	g_mqtt_client = mosquitto_new(client_id, true, NULL);

	if (use_tls) {
		ret = mosquitto_tls_set(g_mqtt_client,
					mqtt_root_ca,
					NULL,
					mqtt_certificate,
					mqtt_private_key,
					NULL);
	}

	mosquitto_max_inflight_messages_set(g_mqtt_client, 20);
	mosquitto_opts_set(g_mqtt_client,
			   MOSQ_OPT_PROTOCOL_VERSION,
			   &protocol_version);

	mosquitto_connect_callback_set(g_mqtt_client, on_connect);
	mosquitto_disconnect_callback_set(g_mqtt_client, on_disconnect);
	mosquitto_message_callback_set(g_mqtt_client, on_message);
	mosquitto_subscribe_callback_set(g_mqtt_client, on_subscribe);

	ret = mosquitto_loop_start(g_mqtt_client);
	if (ret != MOSQ_ERR_SUCCESS) {
		fprintf(stderr, "%s: mosquitto_loop_start %d\n", __func__, ret);
		goto err;
	}

	return 0;
err:
	return -1;
}

int mqtt_forward_start(void)
{
	int ret;

	ret = pthread_create(&mqtt_create_thread,
			     NULL,
			     &create_thread_fn,
			     NULL);
	if (ret) {
		fprintf(stderr, "%s: pthread_create %d\n", __func__, errno);
		goto err;
	}

	if (!server_mode && !list_servers) {
		ret = pthread_create(&tcp_accept_thread,
				     NULL,
				     &tcp_accept_thread_fn,
				     NULL);
		if (ret) {
			fprintf(stderr, "%s: TCP accept: pthread_create %d\n", __func__, errno);
			goto err;
		}
	} else if (!server_mode && list_servers) {
		ret = pthread_create(&beacon_print_thread,
				     NULL,
				     &beacon_print_thread_fn,
				     NULL);
		if (ret) {
			fprintf(stderr, "%s: Server print thread: pthread_create %d\n", __func__, errno);
			goto err;
		}
	} else if (transmit_beacons) {
		ret = pthread_create(&beacon_tx_thread,
				     NULL,
				     &beacon_tx_thread_fn,
				     NULL);
		if (ret) {
			fprintf(stderr, "%s: Beacon: pthread_create %d\n", __func__, errno);
			goto err;
		}

	}

	return 0;
err:
	return ret;
}

void mqtt_forward_wait(void)
{
	if (server_mode)
		(void)pthread_join(mqtt_create_thread, NULL);
	else if (list_servers)
		(void)pthread_join(beacon_print_thread, NULL);
	else
		(void)pthread_join(tcp_accept_thread, NULL);
}

static void print_usage(char *prog_name)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "%s [-d] [-t] [-s] [-p tcp_port] [--client-id client_id] [--mqtt-port mqtt_port] [--mqtt-root-ca root_ca] [--mqtt-certificate cert] [--mqtt-private-key private_key] --mqtt-host mqtt_host --server-side-id server_side_id\n", prog_name);
	fprintf(stderr, "\n");

	fprintf(stderr, "Some options can be set with environment variables:\n");
	fprintf(stderr, "  MQTT_FORWARD_MQTT_HOST       See option --mqtt-host\n");
	fprintf(stderr, "  MQTT_FORWARD_ROOT_CA         See option --mqtt-root-ca\n");
	fprintf(stderr, "  MQTT_FORWARD_CERTIFICATE     See option --mqtt-certificate\n");
	fprintf(stderr, "  MQTT_FORWARD_PRIVATE_KEY     See option --mqtt-private-key\n");
	fprintf(stderr, "  MQTT_FORWARD_REMOTE_IP       See option --remote-ip\n");
	fprintf(stderr, "  MQTT_FORWARD_REMOTE_PORT     See option --remote-port\n");
	fprintf(stderr, "Note: Environment variables will be overridden by command-line options.\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "Optional arguments:\n");
	fprintf(stderr, "  --debug | -d                 Enable debug prints\n");
	fprintf(stderr, "  --tls | -t                   Use MQTT over TLS\n");
	fprintf(stderr, "  --server | -s                Run program on TCP server side\n");
	fprintf(stderr, "                               If not set, program is assumed to run on TCP client side\n");
	fprintf(stderr, "  --port | -p tcp_port         TCP port to forward (client or server port)\n");
	fprintf(stderr, "                               Defaults to 22 if not set\n");
	fprintf(stderr, "                               In server mode (-s): port to connect to on the server\n");
	fprintf(stderr, "                               In client mode: local port to listen on\n");
	fprintf(stderr, "  --remote-port port           Remote TCP port (client mode only)\n");
	fprintf(stderr, "                               Overrides the server's --port setting\n");
	fprintf(stderr, "  --addr | -a address          Address of TCP server (server mode only)\n");
	fprintf(stderr, "                               Defaults to 127.0.0.1 if not set\n");
	fprintf(stderr, "  --remote-ip ip_address       Remote server IP (client mode only)\n");
	fprintf(stderr, "                               Overrides the server's --addr setting\n");
	fprintf(stderr, "  --client-id id               MQTT client ID (client mode only)\n");
	fprintf(stderr, "                               If not set, a random client ID will be generated\n");
	fprintf(stderr, "  --mqtt-port port             Port for MQTT broker\n");
	fprintf(stderr, "                               Defaults: 1883 (non-TLS), 8883 (TLS)\n");
	fprintf(stderr, "  --mqtt-root-ca path          Root CA for MQTT broker (TLS only)\n");
	fprintf(stderr, "  --mqtt-certificate path      Client certificate for MQTT authentication (TLS only)\n");
	fprintf(stderr, "  --mqtt-private-key path      Private key for MQTT authentication (TLS only)\n");
	fprintf(stderr, "  --mqtt-topic-prefix prefix   MQTT topic prefix (default: \"ssh\")\n");
	fprintf(stderr, "  --mqtt-qos qos_level         QoS level for MQTT (default: 1)\n");
	fprintf(stderr, "  --beacon | -b                Transmit beacon frames continuously (server mode only)\n");
	fprintf(stderr, "  --list-server | -l           List available servers (client mode only)\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "Mandatory arguments:\n");
	fprintf(stderr, "  --mqtt-host hostname         Hostname of the MQTT broker\n");
	fprintf(stderr, "  --server-side-id id          Unique ID for the server-side program\n");
	fprintf(stderr, "                               Must match on both client and server for connection\n");
}

int main(int argc, char **argv)
{
	int ret;
	int c;
	int option_index = 0;
	static struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{"debug", no_argument, 0, 'd'},
		{"tls", no_argument, 0, 't'},
		{"server", no_argument, 0, 's'},
		{"list-servers", no_argument, 0, 'l'},
		{"beacon", no_argument, 0, 'b'},
		{"port", 1, 0, 'p'},
		{"addr", 1, 0, 'a'},
		{"remote-port", 1, 0, 1009},
		{"remote-ip", 1, 0, 1010},
		{"client-id", 1, 0, 1005},
		{"server-side-id", 1, 0, 1006},
		{"mqtt-host", 1, 0, 1001},
		{"mqtt-port", 1, 0, 1000},
		{"mqtt-root-ca", 1, 0, 1002},
		{"mqtt-certificate", 1, 0, 1003},
		{"mqtt-private-key", 1, 0, 1004},
		{"mqtt-qos", 1, 0, 1007},
		{"mqtt-topic-prefix", 1, 0, 1008},
		{0, 0, 0, 0}
	};
	int port = 22;
	bool server_id_set = false;
	bool client_id_set = false;
	bool tcp_port_set = false;
	bool mqtt_port_set = false;
	bool mqtt_host_set = false;
	bool mqtt_root_ca_set = false;
	bool mqtt_certificate_set = false;
	bool mqtt_private_key_set = false;
	bool tcp_server_addr_set = false;
	bool mqtt_qos_set = false;
	char *env_var;
	char remote_tcp_server_addr_str[100];

	strncpy(mqtt_topic_prefix, "ssh", sizeof(mqtt_topic_prefix));

	/* Check environment variables before reading command line options*/
	env_var = getenv(ENV_VAR_MQTT_HOST);
	if (env_var) {
		strcpy(mqtt_host, env_var);
		mqtt_host_set = true;
	}
	env_var = getenv(ENV_VAR_ROOT_CA);
	if (env_var) {
		strcpy(mqtt_root_ca, env_var);
		mqtt_root_ca_set = true;
		use_tls = true;
	}
	env_var = getenv(ENV_VAR_CERTIFICATE);
	if (env_var) {
		strcpy(mqtt_certificate, env_var);
		mqtt_certificate_set = true;
		use_tls = true;
	}
	env_var = getenv(ENV_VAR_PRIVATE_KEY);
	if (env_var) {
		strcpy(mqtt_private_key, env_var);
		mqtt_private_key_set = true;
		use_tls = true;
	}
	env_var = getenv(ENV_VAR_TOPIC_PREFIX);
	if (env_var)
		strncpy(mqtt_topic_prefix, env_var, sizeof(mqtt_topic_prefix));
	env_var = getenv(ENV_VAR_REMOTE_IP);
	if (env_var) {
		strcpy(remote_tcp_server_addr_str, env_var);
		remote_tcp_server_addr_set = true;
	}
	env_var = getenv(ENV_VAR_REMOTE_PORT);
	if (env_var) {
		remote_tcp_port = strtol(env_var, NULL, 10);
		remote_tcp_port_set = true;
	}

	while ((c = getopt_long(argc, argv, "hdtslba:p:", long_options, &option_index)) != -1) {
		switch (c) {
		case 'd':
			debug = true;
			break;
		case 't':
			use_tls = true;
			break;
		case 's':
			server_mode = true;
			break;
		case 'l':
			list_servers = true;
			break;
		case 'b':
			transmit_beacons = true;
			break;
		case 1005:
			strcpy(client_mqtt_id, optarg);
			client_id_set = true;
			break;
		case 1006:
			strcpy(server_mqtt_id, optarg);
			server_id_set = true;
			break;
		case 'p':
			port = strtol(optarg, NULL, 10);
			tcp_port_set = true;
			break;
		case 'a':
			strcpy(tcp_server_addr_str, optarg);
			tcp_server_addr_set = true;
			break;
		case 1000:
			mqtt_port = strtol(optarg, NULL, 10);
			mqtt_port_set = true;
			break;
		case 1001:
			strcpy(mqtt_host, optarg);
			mqtt_host_set = true;
			break;
		case 1002:
			strcpy(mqtt_root_ca, optarg);
			mqtt_root_ca_set = true;
			break;
		case 1003:
			strcpy(mqtt_certificate, optarg);
			mqtt_certificate_set = true;
			break;
		case 1004:
			strcpy(mqtt_private_key, optarg);
			mqtt_private_key_set = true;
			break;
		case 1007:
			mqtt_qos = strtol(optarg, NULL, 10);
			mqtt_qos_set = true;
			break;
		case 1008:
			strncpy(mqtt_topic_prefix, optarg, sizeof(mqtt_topic_prefix));
			break;
		case 1009:
			remote_tcp_port = strtol(optarg, NULL, 10);
			remote_tcp_port_set = true;
			break;
		case 1010:
			strcpy(remote_tcp_server_addr_str, optarg);
			remote_tcp_server_addr_set = true;
			break;
		case 'h':
		default:
			print_usage(argv[0]);
			exit(c == 'h' ? 0 : 1);
		}
	}

	if (!server_id_set && (server_mode || !list_servers)) {
		fprintf(stderr, "Missing server ID\n");
		return -1;
	}

	if (!server_mode && !client_id_set) {
		gen_client_id();
		fprintf(stderr, "Missing client ID. Using random ID: %s\n", client_mqtt_id);
	}

	if (!tcp_port_set && !list_servers)
		fprintf(stderr, "Missing TCP port. Using default port %d\n", port);

	if (!mqtt_port_set) {
		mqtt_port = use_tls ? 8883 : 1883;
		fprintf(stderr, "Missing MQTT port. Using default port %d\n", mqtt_port);
	}

	if (server_mode && !tcp_server_addr_set) {
		strcpy(tcp_server_addr_str, "127.0.0.1");
		fprintf(stderr, "Missing server address. Using default addr %s\n", tcp_server_addr_str);
	}

	if (!mqtt_qos_set)
		fprintf(stderr, "Missing MQTT QoS. Using default QoS %d\n", mqtt_qos);

	if (!mqtt_host_set) {
		fprintf(stderr, "Missing MQTT host\n");
		return -1;
	}

	if (use_tls && !mqtt_root_ca_set) {
		fprintf(stderr, "Missing MQTT root ca\n");
		return -1;
	}

	if (use_tls && !mqtt_certificate_set) {
		fprintf(stderr, "Missing MQTT certificate\n");
		return -1;
	}

	if (use_tls && !mqtt_private_key_set) {
		fprintf(stderr, "Missing MQTT private key\n");
		return -1;
	}

	if (remote_tcp_server_addr_set && server_mode) {
		fprintf(stderr, "Remote TCP IP addr set but it is not applicable in server mode\n");
	} else if (remote_tcp_server_addr_set) {
		struct in_addr in_addr;

		ret = inet_aton(remote_tcp_server_addr_str, &in_addr);
		if (ret < 0) {
			fprintf(stderr, "Invalid remote IP addr: %s\n",
				remote_tcp_server_addr_str);
			return -1;
		}
		remote_tcp_server_addr = in_addr.s_addr;
	}

	if (remote_tcp_port_set && server_mode)
		fprintf(stderr, "Remote TCP port set but it is not applicable in server mode\n");

	if (server_mode)
		tcp_server_connect_port = port;
	else
		tcp_client_listen_port = port;

	ret = mqtt_forward_init();
	if (ret < 0)
		return -1;
	mqtt_forward_start();
	if (ret < 0)
		return -1;
	mqtt_forward_wait();

	return 0;
}
