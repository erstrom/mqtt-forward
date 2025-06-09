#ifndef _PROTOCOL_H_
#define _PROTOCOL_H_

#include "session.h"
#include <stdint.h>

enum tcp_over_mqtt_remote_config_type {
	REMOTE_CONFIG_TYPE_IP_ADDR,
	REMOTE_CONFIG_TYPE_PORT,
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


/**
 * Functions
 */

int create_config_header(const struct tcp_session_config *session_cfg,
			 uint8_t **cfg_hdr_buf,
			 size_t *cfg_hdr_size);

#endif
