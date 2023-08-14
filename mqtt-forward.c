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
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <mosquitto.h>
#include <getopt.h>
#include <poll.h>

#define MAX_SESSIONS 100
#define MAX_LIFETIME_SESSIONS (MAX_SESSIONS*100)
#define SESSION_RX_BUF_SIZE 10000
#define SESSION_BACKLOG_SIZE 200
#define TX_WINDOW_LIMIT 100
#define MAX_SERVER_LIST 30

#define DBG_LOG_(fmt, ...) \
	do { \
		if (debug) \
			printf(fmt, __VA_ARGS__); \
	} while (0)

#define ENV_VAR_MQTT_HOST   "MQTT_FORWARD_MQTT_HOST"
#define ENV_VAR_ROOT_CA     "MQTT_FORWARD_ROOT_CA"
#define ENV_VAR_CERTIFICATE "MQTT_FORWARD_CERTIFICATE"
#define ENV_VAR_PRIVATE_KEY "MQTT_FORWARD_PRIVATE_KEY"

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
	char *session_id;
	char *publish_topic;
	int sock;
	uint8_t *rx_buf;
	uint64_t tx_seq_nbr;
	struct rx_packet_backlog rx_backlog;
	struct tx_packet_backlog tx_backlog;
};

struct tcp_over_mqtt_hdr {
	uint64_t seq_nbr;
	uint64_t acked_seq_nbr;
#define TCP_OVER_MQTT_FLAG_TCP_DISCONNECT (1)
#define TCP_OVER_MQTT_FLAG_ACKED_SEQ_NBR (2)
#define TCP_OVER_MQTT_FLAG_NO_DATA (4)
#define TCP_OVER_MQTT_FLAG_BEACON (8)
	uint32_t flags;
	uint32_t pad;
} __attribute__((packed));

struct tcp_server_list {
	char *server_id;
	time_t last_seen_time;
};

static int tcp_client_listen_sock;
static int tcp_client_listen_port;
static int tcp_server_connect_port;
static pthread_t tcp_accept_thread;
static pthread_t beacon_tx_thread;
static pthread_t mqtt_create_thread;
static pthread_t tcp_rx_threads[MAX_SESSIONS];
static struct mosquitto *g_mqtt_client;
static bool connected_to_mqtt_server;
static bool server_mode;
static bool list_servers;
static bool transmit_beacons;
static struct tcp_session tcp_sessions[MAX_SESSIONS];
static struct sockaddr_in *remote_tcp_server_addr;
static char *old_session_ids[MAX_LIFETIME_SESSIONS];
static size_t num_old_sessions;
static bool debug;
static struct tcp_server_list server_list[MAX_SESSIONS];

static pthread_mutex_t session_mtx = PTHREAD_MUTEX_INITIALIZER;

static bool use_tls;
static char server_mqtt_id[100];
static char client_mqtt_id[100];
static int mqtt_port;
static int mqtt_qos = 1;
static char mqtt_host[100];
static char mqtt_root_ca[100];
static char mqtt_certificate[100];
static char mqtt_private_key[100];
static char tcp_server_addr[100];

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
	fprintf(stderr, "%s: TCP session %s terminated\n", __func__, session_data->session_id);
	pthread_mutex_lock(&session_mtx);
	/* Store the session ID of the cleared session in the old session id
	 * array */
	old_session_ids[num_old_sessions++] = session_data->session_id;
	free(session_data->rx_buf);
	free(session_data->publish_topic);
	clear_rx_packet_backlog(&session_data->rx_backlog);
	clear_tx_packet_backlog(&session_data->tx_backlog);
	memset(session_data, 0, sizeof(*session_data));
	pthread_mutex_unlock(&session_mtx);
}

static char *gen_random_id(size_t n_nibbles)
{
	FILE *fp;
	int i;
	uint8_t *id;
	uint8_t nibble_1st;
	uint8_t nibble_2nd;
	char *random_id;

	fp = fopen("/dev/urandom", "rb");
	if (!fp) {
		fprintf(stderr, "%s: unable to open /dev/urandom\n", __func__);
		return NULL;
	}

	random_id = calloc(n_nibbles+1, 1);
	id = calloc(n_nibbles/2, 1);

	(void)fread(id, 1, n_nibbles/2, fp);

	for (i = 0; i < n_nibbles/2; i++) {
		nibble_1st = (id[i] & 0xf0) >> 4;
		nibble_2nd = id[i] & 0x0f;
		nibble_1st = (nibble_1st > 9) ? (nibble_1st - 10 + 'a') : (nibble_1st + '0');
		nibble_2nd = (nibble_2nd > 9) ? (nibble_2nd - 10 + 'a') : (nibble_2nd + '0');
		random_id[2*i] = nibble_1st;
		random_id[2*i+1] = nibble_2nd;
	}

	free(id);
	fclose(fp);

	return random_id;
}

static void gen_client_id()
{
	char *random_id;

	random_id = gen_random_id(12);

	snprintf(client_mqtt_id, sizeof(client_mqtt_id), "mqtt-forward-%s", random_id);
	free(random_id);
}

static char *gen_session_id()
{
	return gen_random_id(32);
}

static void *tcp_session_rx_thread_fn(void *arg)
{
	uint8_t *rx_buf;
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

	for (;;) {
		pthread_mutex_lock(&session_mtx);
		backlog_offset = session_data->tx_seq_nbr - tx_backlog->acked_seq_nbr - 1;

		if (backlog_offset > TX_WINDOW_LIMIT) {
			/* TX window is full. Stop reading more data from input
			 * socket and start re-transmit the lost frame */
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
				DBG_LOG_("Session %s: TX HEARTBEAT: %4lu. Acked %4lu\n",
					 session_data->session_id,
					 tx_hdr.seq_nbr,
					 tx_hdr.acked_seq_nbr);
				ret = mosquitto_publish(g_mqtt_client,
							NULL,
							topic,
							sizeof(tx_hdr),
							&tx_hdr,
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
					rx_buf + sizeof(tx_hdr),
					SESSION_RX_BUF_SIZE - sizeof(tx_hdr),
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
			memcpy(rx_buf, &tx_hdr, sizeof(tx_hdr));
			recv_len += sizeof(tx_hdr);

			/* re-read backlog_offset since acked_seq_nbr might have
			 * been update during the sleep of this thread. */
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
	fprintf(stderr, "%s: TCP session ended\n", __func__);

	clear_session(session_data);

	return NULL;
}

static int create_session(const char *session_id,
			  size_t session_id_len,
			  size_t *session_nbr,
			  int tcp_sock,
			  bool server_session)
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
	 * Clients will create their own ID  */
	if (server_session) {
		session_id_local = calloc(session_id_len + 1, 1);
		strncpy(session_id_local,
			session_id,
			session_id_len);

	} else {
		session_id_local = gen_session_id();
	}

	if (server_session) {
		/* Create TCP socket and connect to the server over loopback
		 * interface */
		tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
		ret = connect(tcp_sock,
			      (const struct sockaddr *) remote_tcp_server_addr,
			      sizeof(*remote_tcp_server_addr));
		if (ret) {
			fprintf(stderr, "%s: Unable to establish TCP connection. errno %d\n",
				  __func__, errno);
			return -1;
		}
	} else {
		/* Subscribe to data from the server for this session_id */
		int mid;
		char topic[200];

		snprintf(topic,
			 sizeof(topic),
			 "ssh/%s/%s/rx",
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
	tcp_sessions[session_nbr_local].session_id = session_id_local;
	tcp_sessions[session_nbr_local].sock = tcp_sock;
	tcp_sessions[session_nbr_local].rx_buf = calloc(SESSION_RX_BUF_SIZE, 1);
	tcp_sessions[session_nbr_local].rx_backlog.expected_seq_nbr = 1;
	tcp_sessions[session_nbr_local].tx_seq_nbr = 1;
	tcp_sessions[session_nbr_local].publish_topic = calloc(200, 1);
	if (server_session)
		snprintf(tcp_sessions[session_nbr_local].publish_topic,
			 200,
			 "ssh/%s/%s/rx",
			 server_mqtt_id,
			 session_id_local);
	else
		snprintf(tcp_sessions[session_nbr_local].publish_topic,
			 200,
			 "ssh/%s/%s/tx",
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

		pthread_mutex_lock(&session_mtx);
		ret = create_session(NULL, /*client session*/
				     0,
				     &session_nbr,
				     client_sock,
				     false); /*client session*/
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
	char topic[200];

	snprintf(topic, sizeof(topic), "ssh/%s/beacon/rx", server_mqtt_id);

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

static void handle_mqtt_message_list_servers(const char *recvd_client_id,
					     size_t recvd_client_id_len)
{
	size_t server_nbr;
	time_t cur_time;

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

	if (server_nbr >= MAX_SERVER_LIST)
		fprintf(stderr, "Server list exhausted!\n");

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
	struct rx_packet_backlog *rx_backlog;
	struct tx_packet_backlog *tx_backlog;
	size_t session_nbr;

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
		 * should create a new session */
		if (!server_session)
			return;

		/* Before a new session is created we must check if the message
		 * belongs to an old session. In this case it should be
		 * discarded */
		for (i = 0; i < num_old_sessions; i++) {
			if (strncmp(old_session_ids[i], session_id, session_id_len) == 0) {
				fprintf(stderr, "%s: Session ID %s is an old session. Message discarded\n",
					__func__, old_session_ids[i]);
				return;
			}
		}

		pthread_mutex_lock(&session_mtx);
		ret = create_session(session_id,
				     session_id_len,
				     &session_nbr,
				     0,
				     true); /*server session*/
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
			 * and should be removed. */
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

	data_len = msg_len - sizeof(*rx_hdr);
	data = msg + sizeof(*rx_hdr);
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
		 * move forward in the backlog until a valid packet is found */
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
	char topic[200];

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
			 200,
			 "ssh/%s/+/tx",
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
			 200,
			 "ssh/+/beacon/rx");

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

int mqtt_forward_init()
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
		ret = getaddrinfo(tcp_server_addr,
				  NULL,
				  &addrhint,
				  &hostaddrinfo);
		if (ret) {
			fprintf(stderr, "%s: getaddrinfo returned %d\n", __func__, ret);
			goto err;
		}

		remote_tcp_server_addr = (struct sockaddr_in *)hostaddrinfo->ai_addr;
		remote_tcp_server_addr->sin_port = htons(22);
	} else {
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

	if (!server_mode) {
		ret = pthread_create(&tcp_accept_thread,
				     NULL,
				     &tcp_accept_thread_fn,
				     NULL);
		if (ret) {
			fprintf(stderr, "%s: TCP accept: pthread_create %d\n", __func__, errno);
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
	else
		(void)pthread_join(tcp_accept_thread, NULL);
}

static void print_usage(char *prog_name)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "%s [-d] [-t] [-s] [-p tcp port] [--client-id client_id] [--mqtt-port mqtt_port] [--mqtt-root-ca rootca] [--mqtt-certificate cert] [--mqtt-private-key privatekey] --mqtt-host mqtt_host --server-side-id server_side_id\n", prog_name);
	fprintf(stderr, "\n");
	fprintf(stderr, "Some of the options can be set with environment variables.\n");
	fprintf(stderr, "Available environment variables are:\n");
	fprintf(stderr, "  MQTT_FORWARD_MQTT_HOST       See option --mqtt-host\n");
	fprintf(stderr, "  MQTT_FORWARD_ROOT_CA         See option --mqtt-private-key\n");
	fprintf(stderr, "  MQTT_FORWARD_CERTIFICATE     See option --mqtt-certificate\n");
	fprintf(stderr, "  MQTT_FORWARD_PRIVATE_KEY     See option --mqtt-private-key\n");
	fprintf(stderr, "The environment variables will be overridden by command line options.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Optional arguments:\n");
	fprintf(stderr, "  --debug|-d  Enable debug prints\n");
	fprintf(stderr, "  --tls|-t    Use MQTT over TLS\n");
	fprintf(stderr, "  --server|-s Run program on TCP server side\n");
	fprintf(stderr, "              If not set, program is assumed to run on TCP client side\n");
	fprintf(stderr, "  --port|-p   TCP port to forward (client or server port).\n");
	fprintf(stderr, "              If -s flag was set, the TCP port refers to the local TCP server port. The program will connect to this port in this case\n");
	fprintf(stderr, "              If -s flag was not set, the TCP port refers to the remote TCP server port. The program will listen to this port in this case\n");
	fprintf(stderr, "              If not set, a default port of 22 will be used\n");
	fprintf(stderr, "  --addr|-a   Address of TCP server (IP address of domain name). Only applicable if --server was used\n");
	fprintf(stderr, "              If not set, a default address of 127.0.0.1 will be used\n");
	fprintf(stderr, "  --client-id MQTT client id. Used as MQTT client id if --server was not used used\n");
	fprintf(stderr, "              If not set, a random client id will be used\n");
	fprintf(stderr, "  --mqtt-port port for MQTT broker.\n");
	fprintf(stderr, "              A default value of 1883 will be used if --tls was not set. 8883 will be used if --tls was set \n");
	fprintf(stderr, "  --mqtt-root-ca  root ca for MQTT broker. Only appicable if --tls was set\n");
	fprintf(stderr, "  --mqtt-certificate  certificate for authentication with MQTT broker. Only appicable if --tls was set\n");
	fprintf(stderr, "  --mqtt-private-key  private key for certificate. Only appicable if --tls was set\n");
	fprintf(stderr, "  --beacon|-b Transmit beacon frames continuosly.\n");
	fprintf(stderr, "              Only applicable for server side (i.e. --server was used).\n");
	fprintf(stderr, "  --list-server|-l  List available servers (provided that the servers are transmitting beacon messages).\n");
	fprintf(stderr, "                    Only applicable for client side (i.e. --server was not used).\n");
	fprintf(stderr, "Mandatory arguments:\n");
	fprintf(stderr, "  --mqtt-host       The host name of the MQTT broker\n");
	fprintf(stderr, "  --server-side-id  A unique string identifying the server side program.\n");
	fprintf(stderr, "                    This is used when the program is run on the server side as well as on the client side.\n");
	fprintf(stderr, "                    The strings must match in order to have a connection between client and server.\n");
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
		{"client-id", 1, 0, 1005},
		{"server-side-id", 1, 0, 1006},
		{"mqtt-host", 1, 0, 1001},
		{"mqtt-port", 1, 0, 1000},
		{"mqtt-root-ca", 1, 0, 1002},
		{"mqtt-certificate", 1, 0, 1003},
		{"mqtt-private-key", 1, 0, 1004},
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
	bool tcp_server_addr_set = false;;
	char *env_var;

	/* Check environment variables before reading command line options*/
	if ((env_var = getenv(ENV_VAR_MQTT_HOST))) {
		strcpy(mqtt_host, env_var);
		mqtt_host_set = true;
	}
	if ((env_var = getenv(ENV_VAR_ROOT_CA))) {
		strcpy(mqtt_root_ca, env_var);
		mqtt_root_ca_set = true;
		use_tls = true;
	}
	if ((env_var = getenv(ENV_VAR_CERTIFICATE))) {
		strcpy(mqtt_certificate, env_var);
		mqtt_certificate_set = true;
		use_tls = true;
	}
	if ((env_var = getenv(ENV_VAR_PRIVATE_KEY))) {
		strcpy(mqtt_private_key, env_var);
		mqtt_private_key_set = true;
		use_tls = true;
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
			strcpy(tcp_server_addr, optarg);
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
		case 'h':
		default:
			print_usage(argv[0]);
			exit(c == 'h' ? 0 : 1);
		}
	}

	if (!server_id_set) {
		fprintf(stderr, "Missing server ID\n");
		return -1;
	}

	if (!server_mode && ! client_id_set) {
		gen_client_id();
		fprintf(stderr, "Missing client ID. Using random ID: %s\n", client_mqtt_id);
	}

	if (!tcp_port_set)
		fprintf(stderr, "Missing TCP port. Using default port %d\n", port);

	if (!mqtt_port_set) {
		mqtt_port = use_tls ? 8883 : 1883;
		fprintf(stderr, "Missing MQTT port. Using default port %d\n", mqtt_port);
	}

	if (server_mode && !tcp_server_addr_set) {
		strcpy(tcp_server_addr, "127.0.0.1");
		fprintf(stderr, "Missing server address. Using default addr %s\n", tcp_server_addr);
	}

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
