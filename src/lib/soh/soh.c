/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file soh.c
 * @brief Implements the MS-SOH parsing code. This is called from rlm_eap_peap
 *
 * @copyright 2010 Phil Mayers (p.mayers@imperial.ac.uk)
 */

RCSID("$Id$")

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/rad_assert.h>
#include "base.h"

/*
 * This code implements parsing of MS-SOH data into FreeRadius AVPs
 * allowing for FreeRadius MS-NAP policies
 */

/**
 * EAP-SOH packet
 */
typedef struct {
	uint16_t tlv_type;	/**< ==7 for EAP-SOH */
	uint16_t tlv_len;
	uint32_t tlv_vendor;

	/**
	 * @name soh-payload
	 * @brief either an soh request or response */
	uint16_t soh_type;	/**< ==2 for request, 1 for response */
	uint16_t soh_len;

	/* an soh-response may now follow... */
} eap_soh;

/**
 * SOH response payload
 * Send by client to server
 */
typedef struct {
	uint16_t outer_type;
	uint16_t outer_len;
	uint32_t vendor;
	uint16_t inner_type;
	uint16_t inner_len;
} soh_response;

/**
 * SOH mode subheader
 * Typical microsoft binary blob nonsense
 */
typedef struct {
	uint16_t outer_type;
	uint16_t outer_len;
	uint32_t vendor;
	uint8_t corrid[24];
	uint8_t intent;
	uint8_t content_type;
} soh_mode_subheader;

/**
 * SOH type-length-value header
 */
typedef struct {
	uint16_t tlv_type;
	uint16_t tlv_len;
} soh_tlv;

static fr_dict_t *dict_freeradius;

extern fr_dict_autoload_t soh_dict[];
fr_dict_autoload_t soh_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_soh_ms_correlation_id;
static fr_dict_attr_t const *attr_soh_ms_health_other;
static fr_dict_attr_t const *attr_soh_ms_machine_name;
static fr_dict_attr_t const *attr_soh_ms_machine_os_build;
static fr_dict_attr_t const *attr_soh_ms_machine_os_release;
static fr_dict_attr_t const *attr_soh_ms_machine_os_vendor;
static fr_dict_attr_t const *attr_soh_ms_machine_os_version;
static fr_dict_attr_t const *attr_soh_ms_machine_processor;
static fr_dict_attr_t const *attr_soh_ms_machine_role;
static fr_dict_attr_t const *attr_soh_ms_machine_sp_release;
static fr_dict_attr_t const *attr_soh_ms_machine_sp_version;
static fr_dict_attr_t const *attr_soh_ms_windows_health_status;

extern fr_dict_attr_autoload_t soh_dict_attr[];
fr_dict_attr_autoload_t soh_dict_attr[] = {
	{ .out = &attr_soh_ms_correlation_id, .name = "SoH-MS-Correlation-Id", .type = FR_TYPE_OCTETS, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_health_other, .name = "SoH-MS-Health-Other", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_name, .name = "SoH-MS-Machine-Name", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_os_build, .name = "SoH-MS-Machine-OS-build", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_os_release, .name = "SoH-MS-Machine-OS-release", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_os_vendor, .name = "SoH-MS-Machine-OS-vendor", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_os_version, .name = "SoH-MS-Machine-OS-version", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_processor, .name = "SoH-MS-Machine-Processor", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_role, .name = "SoH-MS-Machine-Role", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_sp_release, .name = "SoH-MS-Machine-SP-release", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_machine_sp_version, .name = "SoH-MS-Machine-SP-version", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_soh_ms_windows_health_status, .name = "SoH-MS-Windows-Health-Status", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ NULL }
};

static int instance_count = 0;

/** Read big-endian 2-byte unsigned from p
 *
 * caller must ensure enough data exists at "p"
 */
uint16_t soh_pull_be_16(uint8_t const *p) {
	uint16_t r;

	r = *p++ << 8;
	r += *p++;

	return r;
}

/** Read big-endian 3-byte unsigned from p
 *
 * caller must ensure enough data exists at "p"
 */
uint32_t soh_pull_be_24(uint8_t const *p) {
	uint32_t r;

	r = *p++ << 16;
	r += *p++ << 8;
	r += *p++;

	return r;
}

/** Read big-endian 4-byte unsigned from p
 *
 * caller must ensure enough data exists at "p"
 */
uint32_t soh_pull_be_32(uint8_t const *p) {
	uint32_t r;

	r = *p++ << 24;
	r += *p++ << 16;
	r += *p++ << 8;
	r += *p++;

	return r;
}

static int eap_peap_soh_mstlv(REQUEST *request, uint8_t const *p, unsigned int data_len) CC_HINT(nonnull);

/** Parses the MS-SOH type/value (note: NOT type/length/value) data and update the sohvp list
 *
 * See section 2.2.4 of MS-SOH. Because there's no "length" field we CANNOT just skip
 * unknown types; we need to know their length ahead of time. Therefore, we abort
 * if we find an unknown type. Note that sohvp may still have been modified in the
 * failure case.
 *
 * @param request Current request
 * @param p binary blob
 * @param data_len length of blob
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int eap_peap_soh_mstlv(REQUEST *request, uint8_t const *p, unsigned int data_len)
{
	VALUE_PAIR *vp;
	uint8_t c;
	int t;

	while (data_len > 0) {
		c = *p++;
		data_len--;

		switch (c) {
		/*
	         *	MS-Machine-Inventory-Packet
		 *	MS-SOH section 2.2.4.1
		 */
		case 1:
			if (data_len < 18) {
				RDEBUG2("insufficient data for MS-Machine-Inventory-Packet");
				return 0;
			}
			data_len -= 18;

			MEM(pair_update_request(&vp, attr_soh_ms_machine_os_vendor) >= 0);
			fr_pair_value_from_str(vp, "Microsoft", -1, '\0', false);

			MEM(pair_update_request(&vp, attr_soh_ms_machine_os_version) >= 0);
			vp->vp_uint32 = soh_pull_be_32(p);
			p += 4;

			MEM(pair_update_request(&vp, attr_soh_ms_machine_os_release) >= 0);
			vp->vp_uint32 = soh_pull_be_32(p);
			p += 4;

			MEM(pair_update_request(&vp, attr_soh_ms_machine_os_build) >= 0);
			vp->vp_uint32 = soh_pull_be_32(p);
			p += 4;

			MEM(pair_update_request(&vp, attr_soh_ms_machine_sp_version) >= 0);
			vp->vp_uint32 = soh_pull_be_16(p);
			p += 2;

			MEM(pair_update_request(&vp, attr_soh_ms_machine_sp_release) >= 0);
			vp->vp_uint32 = soh_pull_be_16(p);
			p += 2;

			MEM(pair_update_request(&vp, attr_soh_ms_machine_processor) >= 0);
			vp->vp_uint32 = soh_pull_be_16(p);
			p += 2;
			break;
		/*
		 *	MS-Quarantine-State - FIXME: currently unhandled
		 *	MS-SOH 2.2.4.1
		 *
		 *	1 byte reserved
		 *	1 byte flags
		 *	8 bytes NT Time field (100-nanosec since 1 Jan 1601)
		 *	2 byte urilen
		 *	N bytes uri
		 */
		case 2:

			p += 10;
			t = soh_pull_be_16(p);	/* t == uri len */
			p += 2;
			p += t;
			data_len -= 12 + t;
			break;

		/*
		 *	MS-Packet-Info
		 *	MS-SOH 2.2.4.3
		 */
		case 3:

			RDEBUG3("SoH MS-Packet-Info %s vers=%i", (*p & 0x10) ? "request" : "response", *p & 0xf);
			p++;
			data_len--;
			break;

		/*
		 *	MS-SystemGenerated-Ids - FIXME: currently unhandled
		 *	MS-SOH 2.2.4.4
		 *
		 *	2 byte length
		 *	N bytes (3 bytes IANA enterprise# + 1 byte component id#)
		 */
		case 4:

			t = soh_pull_be_16(p);
			p += 2;
			p += t;
			data_len -= 2 + t;
			break;

		/*
		 *	MS-MachineName
		 *	MS-SOH 2.2.4.5
		 *
		 *	1 byte namelen
		 *	N bytes name
		 */
		case 5:

			t = soh_pull_be_16(p);
			p += 2;

			MEM(pair_update_request(&vp, attr_soh_ms_machine_name) >= 0);
			fr_pair_value_bstrncpy(vp, p, t);

			p += t;
			data_len -= 2 + t;
			break;

		/*
		 *	MS-CorrelationId
		 *	MS-SOH 2.2.4.6
		 *
		 *	24 bytes opaque binary which we might, in future, have
		 *	to echo back to the client in a final SoHR
		 */
		case 6:
			MEM(pair_update_request(&vp, attr_soh_ms_correlation_id) >= 0);
			fr_pair_value_memcpy(vp, p, 24, true);
			p += 24;
			data_len -= 24;
			break;

		/*
		 *	MS-Installed-Shvs - FIXME: currently unhandled
		 *	MS-SOH 2.2.4.7
		 *
		 *	2 bytes length
		 *	N bytes (3 bytes IANA enterprise# + 1 byte component id#)
		 */
		case 7:
			t = soh_pull_be_16(p);
			p += 2;
			p += t;
			data_len -= 2 + t;
			break;

		/*
		 *	MS-Machine-Inventory-Ex
		 *	MS-SOH 2.2.4.8
		 *
		 *	4 bytes reserved
		 *	1 byte product type (client=1 domain_controller=2 server=3)
		 */
		case 8:
			p += 4;
			MEM(pair_update_request(&vp, attr_soh_ms_machine_role));
			vp->vp_uint32 = *p;
			p++;
			data_len -= 5;
			break;

		default:
			RDEBUG2("SoH Unknown MS TV %i stopping", c);
			return 0;
		}
	}
	return 1;
}
/** Convert windows Health Class status into human-readable string
 *
 * Tedious, really, really tedious...
 */
static char const *clientstatus2str(uint32_t hcstatus) {
	switch (hcstatus) {
	/* this lot should all just be for windows updates */
	case 0xff0005:
		return "wua-ok";

	case 0xff0006:
		return "wua-missing";

	case 0xff0008:
		return "wua-not-started";

	case 0xc0ff000c:
		return "wua-no-wsus-server";

	case 0xc0ff000d:
		return "wua-no-wsus-clientid";

	case 0xc0ff000e:
		return "wua-disabled";

	case 0xc0ff000f:
		return "wua-comm-failure";

	/* these next 3 are for all health-classes */
	case 0xc0ff0002:
		return "not-installed";

	case 0xc0ff0003:
		return "down";

	case 0xc0ff0018:
		return "not-started";
	}
	return NULL;
}

/** Convert a Health Class into a string
 *
 */
static char const *healthclass2str(uint8_t hc) {
	switch (hc) {
	case 0:
		return "firewall";

	case 1:
		return "antivirus";

	case 2:
		return "antispyware";

	case 3:
		return "updates";

	case 4:
		return "security-updates";
	}
	return NULL;
}

/** Parse the MS-SOH response in data and update sohvp
 *
 * Note that sohvp might still have been updated in event of a failure.
 *
 * @param request Current request
 * @param data MS-SOH blob
 * @param data_len length of MS-SOH blob
 *
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int soh_verify(REQUEST *request, uint8_t const *data, unsigned int data_len) {

	VALUE_PAIR		*vp;
	eap_soh			hdr;
	soh_response		resp;
	soh_mode_subheader	mode;
	soh_tlv			tlv;
	int			curr_shid =- 1, curr_shid_c =- 1, curr_hc =- 1;

	rad_assert(request->packet != NULL);

	hdr.tlv_type = soh_pull_be_16(data); data += 2;
	hdr.tlv_len = soh_pull_be_16(data); data += 2;
	hdr.tlv_vendor = soh_pull_be_32(data); data += 4;

	if (hdr.tlv_type != 7 || hdr.tlv_vendor != 0x137) {
		RDEBUG2("SoH payload is %i %08x not a ms-vendor packet", hdr.tlv_type, hdr.tlv_vendor);
		return -1;
	}

	hdr.soh_type = soh_pull_be_16(data); data += 2;
	hdr.soh_len = soh_pull_be_16(data); data += 2;
	if (hdr.soh_type != 1) {
		RDEBUG2("SoH tlv %04x is not a response", hdr.soh_type);
		return -1;
	}

	/* FIXME: check for sufficient data */
	resp.outer_type = soh_pull_be_16(data); data += 2;
	resp.outer_len = soh_pull_be_16(data); data += 2;
	resp.vendor = soh_pull_be_32(data); data += 4;
	resp.inner_type = soh_pull_be_16(data); data += 2;
	resp.inner_len = soh_pull_be_16(data); data += 2;


	if ((resp.outer_type != 7) || (resp.vendor != 0x137)) {
		RDEBUG2("SoH response outer type %i/vendor %08x not recognised", resp.outer_type, resp.vendor);
		return -1;
	}
	switch (resp.inner_type) {
	case 1:
		/* no mode sub-header */
		RDEBUG2("SoH without mode subheader");
		break;

	case 2:
		mode.outer_type = soh_pull_be_16(data); data += 2;
		mode.outer_len = soh_pull_be_16(data); data += 2;
		mode.vendor = soh_pull_be_32(data); data += 4;
		memcpy(mode.corrid, data, 24); data += 24;
		mode.intent = data[0];
		mode.content_type = data[1];
		data += 2;

		if ((mode.outer_type != 7) || (mode.vendor != 0x137) || (mode.content_type != 0)) {
			RDEBUG3("SoH mode subheader outer type %i/vendor %08x/content type %i invalid",
				mode.outer_type, mode.vendor, mode.content_type);
			return -1;
		}
		RDEBUG3("SoH with mode subheader");
		break;

	default:
		RDEBUG2("SoH invalid inner type %i", resp.inner_type);
		return -1;
	}

	/* subtract off the relevant amount of data */
	if (resp.inner_type==2) {
		data_len = resp.inner_len - 34;
	} else {
		data_len = resp.inner_len;
	}

	/* TLV
	 * MS-SOH 2.2.1
	 * See also 2.2.3
	 *
	 * 1 bit mandatory
	 * 1 bit reserved
	 * 14 bits tlv type
	 * 2 bytes tlv length
	 * N bytes payload
	 *
	 */
	while (data_len >= 4) {
		tlv.tlv_type = soh_pull_be_16(data); data += 2;
		tlv.tlv_len = soh_pull_be_16(data); data += 2;

		data_len -= 4;

		switch (tlv.tlv_type) {
		case 2:
			/* System-Health-Id TLV
			 * MS-SOH 2.2.3.1
			 *
			 * 3 bytes IANA/SMI vendor code
			 * 1 byte component (i.e. within vendor, which SoH component
			 */
			curr_shid = soh_pull_be_24(data);
			curr_shid_c = data[3];
			RDEBUG2("SoH System-Health-ID vendor %08x component=%i", curr_shid, curr_shid_c);
			break;

		case 7:
			/* Vendor-Specific packet
			 * MS-SOH 2.2.3.3
			 *
			 * 4 bytes vendor, supposedly ignored by NAP
			 * N bytes payload; for Microsoft component#0 this is the MS TV stuff
			 */
			if (curr_shid==0x137 && curr_shid_c==0) {
				RDEBUG2("SoH MS type-value payload");
				eap_peap_soh_mstlv(request, data + 4, tlv.tlv_len - 4);
			} else {
				RDEBUG2("SoH unhandled vendor-specific TLV %08x/component=%i %i bytes payload",
					curr_shid, curr_shid_c, tlv.tlv_len);
			}
			break;

		case 8:
			/* Health-Class
			 * MS-SOH 2.2.3.5.6
			 *
			 * 1 byte integer
			 */
			RDEBUG2("SoH Health-Class %i", data[0]);
			curr_hc = data[0];
			break;

		case 9:
			/* Software-Version
			 * MS-SOH 2.2.3.5.7
			 *
			 * 1 byte integer
			 */
			RDEBUG2("SoH Software-Version %i", data[0]);
			break;

		case 11:
			/* Health-Class status
			 * MS-SOH 2.2.3.5.9
			 *
			 * variable data; for the MS System Health vendor, these are 4-byte
			 * integers which are a really, really dumb format:
			 *
			 *  28 bits ignore
			 *  1 bit - 1==product snoozed
			 *  1 bit - 1==microsoft product
			 *  1 bit - 1==product up-to-date
			 *  1 bit - 1==product enabled
			 */
			RDEBUG2("SoH Health-Class-Status - current shid=%08x component=%i", curr_shid, curr_shid_c);

			if (curr_shid == 0x137 && curr_shid_c == 128) {
				char const *s, *t;
				uint32_t hcstatus = soh_pull_be_32(data);

				RDEBUG2("SoH Health-Class-Status microsoft DWORD=%08x", hcstatus);

				MEM(pair_update_request(&vp, attr_soh_ms_windows_health_status) >= 0);
				switch (curr_hc) {
				case 4:
					/* security updates */
					s = "security-updates";
					switch (hcstatus) {
					case 0xff0005:
						fr_pair_value_snprintf(vp, "%s ok all-installed", s);
						break;

					case 0xff0006:
						fr_pair_value_snprintf(vp, "%s warn some-missing", s);
						break;

					case 0xff0008:
						fr_pair_value_snprintf(vp, "%s warn never-started", s);
						break;

					case 0xc0ff000c:
						fr_pair_value_snprintf(vp, "%s error no-wsus-srv", s);
						break;

					case 0xc0ff000d:
						fr_pair_value_snprintf(vp, "%s error no-wsus-clid", s);
						break;

					case 0xc0ff000e:
						fr_pair_value_snprintf(vp, "%s warn wsus-disabled", s);
						break;

					case 0xc0ff000f:
						fr_pair_value_snprintf(vp, "%s error comm-failure", s);
						break;

					case 0xc0ff0010:
						fr_pair_value_snprintf(vp, "%s warn needs-reboot", s);
						break;

					default:
						fr_pair_value_snprintf(vp, "%s error %08x", s, hcstatus);
						break;
					}
					break;

				case 3:
					/* auto updates */
					s = "auto-updates";
					switch (hcstatus) {
					case 1:
						fr_pair_value_snprintf(vp, "%s warn disabled", s);
						break;

					case 2:
						fr_pair_value_snprintf(vp, "%s ok action=check-only", s);
						break;

					case 3:
						fr_pair_value_snprintf(vp, "%s ok action=download", s);
						break;

					case 4:
						fr_pair_value_snprintf(vp, "%s ok action=install", s);
						break;

					case 5:
						fr_pair_value_snprintf(vp, "%s warn unconfigured", s);
						break;

					case 0xc0ff0003:
						fr_pair_value_snprintf(vp, "%s warn service-down", s);
						break;

					case 0xc0ff0018:
						fr_pair_value_snprintf(vp, "%s warn never-started", s);
						break;

					default:
						fr_pair_value_snprintf(vp, "%s error %08x", s, hcstatus);
						break;
					}
					break;

				default:
					/* other - firewall, antivirus, antispyware */
					s = healthclass2str(curr_hc);
					if (s) {
						/* bah. this is vile. stupid microsoft
						 */
						if (hcstatus & 0xff000000) {
							/* top octet non-zero means an error
							 * FIXME: is this always correct? MS-WSH 2.2.8 is unclear
							 */
							t = clientstatus2str(hcstatus);
							if (t) {
								fr_pair_value_snprintf(vp, "%s error %s", s, t);
							} else {
								fr_pair_value_snprintf(vp, "%s error %08x", s, hcstatus);
							}
						} else {
							fr_pair_value_snprintf(vp,
									"%s ok snoozed=%i microsoft=%i up2date=%i enabled=%i",
									s,
									(hcstatus & 0x8) ? 1 : 0,
									(hcstatus & 0x4) ? 1 : 0,
									(hcstatus & 0x2) ? 1 : 0,
									(hcstatus & 0x1) ? 1 : 0
									);
						}
					} else {
						fr_pair_value_snprintf(vp, "%i unknown %08x", curr_hc, hcstatus);
					}
					break;
				}
			} else {
				MEM(pair_update_request(&vp, attr_soh_ms_health_other) >= 0);

				/* FIXME: what to do with the payload? */
				fr_pair_value_snprintf(vp, "%08x/%i ?", curr_shid, curr_shid_c);
			}
			break;

		default:
			RDEBUG2("SoH Unknown TLV %i len=%i", tlv.tlv_type, tlv.tlv_len);
			break;
		}

		data += tlv.tlv_len;
		data_len -= tlv.tlv_len;
	}

	return 0;
}

int fr_soh_init(void)
{
	if (instance_count > 0) {
		instance_count++;
		return 0;
	}

	if (fr_dict_autoload(soh_dict) < 0) return -1;

	if (fr_dict_attr_autoload(soh_dict_attr) < 0) {
		fr_dict_autofree(soh_dict);
		return -1;
	}

	instance_count++;

	return 0;
}

void fr_soh_free(void)
{
	if (--instance_count > 0) return;

	fr_dict_autofree(soh_dict);
}
