#include "utils.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <openssl/rand.h>

char *gen_random_id(size_t n_nibbles)
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

void gen_client_id(char *client_mqtt_id, size_t client_mqtt_id_len)
{
	char *random_id;

	random_id = gen_random_id(12);

	snprintf(client_mqtt_id,
		 client_mqtt_id_len,
		 "mqtt-forward-%s",
		 random_id);
	free(random_id);
}

char *gen_session_id(void)
{
	return gen_random_id(32);
}
