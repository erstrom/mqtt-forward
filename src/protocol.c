#include "protocol.h"
#include <stdlib.h>
#include <stdio.h>

int create_config_header(const struct tcp_session_config *session_cfg,
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


