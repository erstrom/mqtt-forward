#ifndef _UTILS_H_
#define _UTILS_H_

#include <stdbool.h>
#include <stddef.h>

char *gen_random_id(size_t n_nibbles);
void gen_client_id(char *client_mqtt_id, size_t client_mqtt_id_len);
char *gen_session_id(void);
bool sized_str_eq(const char *str, const char *buf, size_t buf_len);

#endif
