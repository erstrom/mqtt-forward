#include "beacon.h"
#include "session.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MAX_SERVER_LIST 30

struct tcp_server_list {
	char *server_id;
	time_t last_seen_time;
};

static struct tcp_server_list server_list[MAX_SESSIONS];
static pthread_mutex_t server_list_mtx = PTHREAD_MUTEX_INITIALIZER;

void beacon_add_server_to_list(const char *recvd_client_id,
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

void beacon_print_server_list(void)
{
	size_t server_nbr;
	time_t cur_time;

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
