#include "session.h"
#include "utils.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>


struct tcp_session tcp_sessions[MAX_SESSIONS];
pthread_mutex_t session_mtx = PTHREAD_MUTEX_INITIALIZER;
char *old_session_ids[MAX_LIFETIME_SESSIONS];
size_t num_old_sessions;

static pthread_t tcp_rx_threads[MAX_SESSIONS];

static int connect_server_session(const struct tcp_session_config *session_cfg,
				  struct sockaddr_in *tcp_server_addr,
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


void clear_rx_packet_backlog(struct rx_packet_backlog *rx_backlog)
{
	int i;

	for (i = 0; i < SESSION_BACKLOG_SIZE; i++)
		free(rx_backlog->backlog[i].buf);

	memset(&rx_backlog->backlog[0],
	       0,
	       sizeof(rx_backlog->backlog[0])*SESSION_BACKLOG_SIZE);
}

void clear_tx_packet_backlog(struct tx_packet_backlog *tx_backlog)
{
	int i;

	for (i = 0; i < SESSION_BACKLOG_SIZE; i++)
		free(tx_backlog->backlog[i].buf);

	memset(&tx_backlog->backlog[0],
	       0,
	       sizeof(tx_backlog->backlog[0])*SESSION_BACKLOG_SIZE);
}

void clear_session(struct tcp_session *session_data)
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
		   void *(*thread_fn)(void *))
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
		ret = connect_server_session(session_cfg, tcp_server_addr, &tcp_sock);
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

		ret = mosquitto_subscribe(mqtt_client,
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
			     thread_fn,
			     &tcp_sessions[session_nbr_local]);
	if (ret) {
		fprintf(stderr, "%s: pthread_create %d\n", __func__, errno);
		return -1;
	}

	*session_nbr = session_nbr_local;

	return 0;
}


