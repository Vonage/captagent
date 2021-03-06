/*
 * $Id$
 *
 *  captagent - Homer capture agent. Modular
 *  Duplicate SIP messages in Homer Encapulate Protocol [HEP] [ipv6 version]
 *
 *  Author: Holger Hans Peter Freyther <help@moiji-mobile.com>
 *  (C) Homer Project 2016 (http://www.sipcapture.org)
 *
 * Homer capture agent is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version
 *
 * Homer capture agent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
*/

#include <captagent/api.h>
#include <captagent/structure.h>
#include <captagent/modules_api.h>
#include <captagent/modules.h>
#include <captagent/log.h>

#include <endian.h>
#include <limits.h>


#define SCTP_M2UA_PPID	2

#define M2UA_MSG	6
#define M2UA_DATA	1

#define M2UA_IE_DATA	0x0300

#define MTP_ISUP	0x05

struct mtp_level_3_hdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t ser_ind : 4,
		spare : 2,
		ni : 2;
	uint32_t dpc : 14,
		opc : 14,
		sls : 4;
#elif __BYTE_ORDER == __BIG_ENDIAN
	uint8_t ni : 2,
		spare : 2,
		ser_ind : 4;
	uint32_t sls : 4,
		opc : 14,
		dpc : 14;
#else
	#error "Unknonwn endian type"
#endif
	uint8_t data[0];
} __attribute__((packed));

static int ss7_parse_isup(msg_t *, char *, char *);
static int ss7_load_module(xml_node *config);
static int ss7_unload_module(void);
static int ss7_description(char *description);
static int ss7_statistic(char *buf, size_t len);
static uint64_t ss7_serial_module(void);

static uint64_t module_serial = 0;

static cmd_export_t ss7_cmds[] = {
	{
		.name		= "parse_isup",
		.function	= ss7_parse_isup,
		.param_no	= 0,
		.flags		= 0,
		.fixup_flags	= 0,
	},
	{ 0, },
};

struct module_exports exports = {
	.name		= "protocol_ss7",
        .cmds		= ss7_cmds,
        .load_f		= ss7_load_module,
        .unload_f	= ss7_unload_module,
        .description_f	= ss7_description,
        .stats_f	= ss7_statistic,
        .serial_f	= ss7_serial_module,
};

static uint8_t *extract_from_m2ua(msg_t *msg, size_t *len)
{
	uint8_t *data;
	uint32_t data_len;

	if (msg->len < 8) {
		LERR("M2UA hdr too short %u", msg->len);
		return NULL;
	}
	data = msg->data;

	/* check the header */
	if (data[0] != 0x01) {
		LERR("M2UA unknown version number %d", data[0]);
		return NULL;
	}
	if (data[1] != 0x00) {
		LERR("M2UA unknown reserved fields %d", data[1]);
		return NULL;
	}
	if (data[2] != M2UA_MSG) {
		LDEBUG("M2UA unhandled message class %d", data[2]);
		return NULL;
	}
	if (data[3] != M2UA_DATA) {
		LDEBUG("M2UA not data msg but %d", data[3]);
		return NULL;
	}

	/* check the length */
	memcpy(&data_len, &data[4], sizeof(data_len));
	data_len = ntohl(data_len);
	if (msg->len < data_len) {
		LERR("M2UA data can't fit %u vs. %u", msg->len, data_len);
		return NULL;
	}

	/* skip the header */
	data += 8;
	data_len -= 8;
	while (data_len > 4) {
		uint16_t ie_tag, ie_len, padding;
		memcpy(&ie_tag, &data[0], sizeof(ie_tag));
		memcpy(&ie_len, &data[2], sizeof(ie_len));
		ie_tag = ntohs(ie_tag);
		ie_len = ntohs(ie_len);

		if (ie_len > data_len) {
			LERR("M2UA premature end %u vs. %u", ie_len, data_len);
			return NULL;
		}

		if (ie_tag != M2UA_IE_DATA)
			goto next;

		*len = ie_len - 4;
		return &data[4];

next:
		data += ie_len;
		data_len -= ie_len;

		/* and now padding... */
                padding = (4 - (ie_len % 4)) & 0x3;
		if (data_len < padding) {
			LERR("M2UA no place for padding %u vs. %u", padding, data_len);
			return NULL;
		}
		data += padding;
		data_len -= padding;
	}
	/* No data IE was found */
	LERR("M2UA no data element found");
	return NULL;
}

static uint8_t *extract_from_mtp(uint8_t *data, size_t *len, int *opc, int *dpc, int *type)
{
	struct mtp_level_3_hdr *hdr;

	*opc = INT_MAX;
	*dpc = INT_MAX;

	if (!data)
		return NULL;
	if (*len < sizeof(*hdr)) {
		LERR("MTP not enough space for mtp hdr %zu vs. %zu", *len, sizeof(*hdr));
		return NULL;
	}

	hdr = (struct mtp_level_3_hdr *) data;
	*opc = hdr->opc;
	*dpc = hdr->dpc;
	*type = hdr->ser_ind;
	*len -= sizeof(*hdr);
	return &hdr->data[0];
}

static uint8_t *ss7_extract_payload(msg_t *msg, size_t *len, int *opc, int *dpc, int *type)
{
	switch (msg->sctp_ppid) {
	case SCTP_M2UA_PPID:
		msg->rcinfo.proto_type = 0x08;
		return extract_from_mtp(extract_from_m2ua(msg, len), len, opc, dpc, type);
		break;
	default:
		LDEBUG("SS7 SCTP PPID(%u) not known", msg->sctp_ppid);
		return NULL;
	}
}

static int ss7_parse_isup(msg_t *msg, char *param1, char *param2)
{
	uint8_t *data;
	size_t len;
	int opc, dpc, type;

	data = ss7_extract_payload(msg, &len, &opc, &dpc, &type);
	if (!data)
		return -1;
	if (type != MTP_ISUP) {
		LDEBUG("ISUP service indicator not ISUP but %d", type);
		return -1;
	}

	/* data[0:1] is now the CIC and data[2] the type */
	return 1;
}

static int ss7_load_module(xml_node *config)
{
	return 0;
}

static int ss7_unload_module(void)
{
	return 0;
}

static int ss7_description(char *description)
{
	return 1;
}

static int ss7_statistic(char *buf, size_t len)
{
	return 1;
}

static uint64_t ss7_serial_module(void)
{
	return module_serial;
}
