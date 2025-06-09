#ifndef _UTILS_H_
#define _UTILS_H_

#include <stddef.h>

char *gen_random_id(size_t n_nibbles);
void gen_client_id(char *client_mqtt_id, size_t client_mqtt_id_len);
char *gen_session_id(void);

#endif
