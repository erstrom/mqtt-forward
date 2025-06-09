#ifndef _BEACON_H_
#define _BEACON_H_

#include <stddef.h>

void beacon_add_server_to_list(const char *recvd_client_id,
			       size_t recvd_client_id_len);

void beacon_print_server_list(void);

#endif
