/*
 * Copyright (c) 2017-2021 Nordic Semiconductor ASA
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/buf.h>

#include "common/bt_str.h"

#include "host/hci_core.h"
#include "host/conn_internal.h"
#include "host/keys.h"
#include "sco_internal.h"

#define LOG_LEVEL CONFIG_BT_HCI_CORE_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_br);

#define RSSI_INVALID 127

enum __packed resolve_name_state {
	RESOLVE_REMOTE_NAME_PENDING,
	RESOLVE_REMOTE_NAME_RESOLVING,
	RESOLVE_REMOTE_NAME_RESOLVED,
};

struct bt_br_discovery_result *discovery_results;
static size_t discovery_results_size;
static size_t discovery_results_count;
static sys_slist_t discovery_cbs = SYS_SLIST_STATIC_INIT(&discovery_cbs);

static int reject_conn(const bt_addr_t *bdaddr, uint8_t reason)
{
	struct bt_hci_cp_reject_conn_req *cp;
	struct net_buf *buf;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	bt_addr_copy(&cp->bdaddr, bdaddr);
	cp->reason = reason;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_REJECT_CONN_REQ, buf, NULL);
	if (err) {
		return err;
	}

	return 0;
}

static int accept_conn(const bt_addr_t *bdaddr)
{
	struct bt_hci_cp_accept_conn_req *cp;
	struct net_buf *buf;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	bt_addr_copy(&cp->bdaddr, bdaddr);
	cp->role = BT_HCI_ROLE_PERIPHERAL;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_ACCEPT_CONN_REQ, buf, NULL);
	if (err) {
		return err;
	}

	return 0;
}

void bt_hci_conn_req(struct net_buf *buf)
{
	struct bt_hci_evt_conn_request *evt = (void *)buf->data;
	struct bt_conn *conn;

	LOG_DBG("conn req from %s, type 0x%02x", bt_addr_str(&evt->bdaddr), evt->link_type);

	if (evt->link_type != BT_HCI_ACL) {
		uint8_t err;

		err = bt_esco_conn_req(evt);
		if (err != BT_HCI_ERR_SUCCESS) {
			reject_conn(&evt->bdaddr, err);
		}
		return;
	}

	conn = bt_conn_add_br(&evt->bdaddr);
	if (!conn) {
		reject_conn(&evt->bdaddr, BT_HCI_ERR_INSUFFICIENT_RESOURCES);
		return;
	}

	accept_conn(&evt->bdaddr);
	conn->role = BT_HCI_ROLE_PERIPHERAL;
	bt_conn_set_state(conn, BT_CONN_INITIATING);
	bt_conn_unref(conn);
}

static bool br_sufficient_key_size(struct bt_conn *conn)
{
	struct bt_hci_cp_read_encryption_key_size *cp;
	struct bt_hci_rp_read_encryption_key_size *rp;
	struct net_buf *buf, *rsp;
	uint8_t key_size;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		LOG_ERR("Failed to allocate command buffer");
		return false;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(conn->handle);

	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_ENCRYPTION_KEY_SIZE, buf, &rsp);
	if (err) {
		LOG_ERR("Failed to read encryption key size (err %d)", err);
		return false;
	}

	if (rsp->len < sizeof(*rp)) {
		LOG_ERR("Too small command complete for encryption key size");
		net_buf_unref(rsp);
		return false;
	}

	rp = (void *)rsp->data;
	key_size = rp->key_size;
	net_buf_unref(rsp);

	LOG_DBG("Encryption key size is %u", key_size);

	if (conn->sec_level == BT_SECURITY_L4) {
		return key_size == BT_HCI_ENCRYPTION_KEY_SIZE_MAX;
	}

	return key_size >= CONFIG_BT_BR_MIN_ENC_KEY_SIZE;
}

bool bt_br_update_sec_level(struct bt_conn *conn)
{
	if (!conn->encrypt) {
		conn->sec_level = BT_SECURITY_L1;
		return true;
	}

	if (conn->br.link_key) {
		if (conn->br.link_key->flags & BT_LINK_KEY_AUTHENTICATED) {
			if (conn->encrypt == BT_HCI_ENCRYPTION_ON_BR_AES_CCM) {
				conn->sec_level = BT_SECURITY_L4;
			} else {
				conn->sec_level = BT_SECURITY_L3;
			}
		} else {
			conn->sec_level = BT_SECURITY_L2;
		}
	} else {
		LOG_WRN("No BR/EDR link key found");
		conn->sec_level = BT_SECURITY_L2;
	}

	if (!br_sufficient_key_size(conn)) {
		LOG_ERR("Encryption key size is not sufficient");
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return false;
	}

	if (conn->required_sec_level > conn->sec_level) {
		LOG_ERR("Failed to set required security level");
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return false;
	}

	return true;
}

void bt_hci_synchronous_conn_complete(struct net_buf *buf)
{
	struct bt_hci_evt_sync_conn_complete *evt = (void *)buf->data;
	struct bt_conn *sco_conn;
	uint16_t handle = sys_le16_to_cpu(evt->handle);

	LOG_DBG("status 0x%02x, handle %u, type 0x%02x", evt->status, handle, evt->link_type);

	sco_conn = bt_conn_lookup_addr_sco(&evt->bdaddr);
	if (!sco_conn) {
		LOG_ERR("Unable to find conn for %s", bt_addr_str(&evt->bdaddr));
		return;
	}

	if (evt->status) {
		sco_conn->err = evt->status;
		bt_conn_set_state(sco_conn, BT_CONN_DISCONNECTED);
		bt_conn_unref(sco_conn);
		return;
	}

	sco_conn->handle = handle;
	bt_conn_set_state(sco_conn, BT_CONN_CONNECTED);
	bt_conn_unref(sco_conn);
}

void bt_hci_conn_complete(struct net_buf *buf)
{
	struct bt_hci_evt_conn_complete *evt = (void *)buf->data;
	struct bt_conn *conn;
	struct bt_hci_cp_read_remote_features *cp;
	uint16_t handle = sys_le16_to_cpu(evt->handle);

	LOG_DBG("status 0x%02x, handle %u, type 0x%02x", evt->status, handle, evt->link_type);

	conn = bt_conn_lookup_addr_br(&evt->bdaddr);
	if (!conn) {
		LOG_ERR("Unable to find conn for %s", bt_addr_str(&evt->bdaddr));
		return;
	}

	if (evt->status) {
		conn->err = evt->status;
		bt_conn_set_state(conn, BT_CONN_DISCONNECTED);
		bt_conn_unref(conn);
		return;
	}

	conn->handle = handle;
	conn->err = 0U;
	conn->encrypt = evt->encr_enabled;

	if (!bt_br_update_sec_level(conn)) {
		bt_conn_unref(conn);
		return;
	}

	bt_conn_set_state(conn, BT_CONN_CONNECTED);

	atomic_set_bit_to(conn->flags, BT_CONN_BR_BONDABLE, bt_get_bondable());

	bt_conn_connected(conn);

	bt_conn_unref(conn);

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = evt->handle;

	bt_hci_cmd_send_sync(BT_HCI_OP_READ_REMOTE_FEATURES, buf, NULL);
}

static int request_name(const bt_addr_t *addr, uint8_t pscan, uint16_t offset)
{
	struct bt_hci_cp_remote_name_request *cp;
	struct net_buf *buf;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	cp = net_buf_add(buf, sizeof(*cp));

	bt_addr_copy(&cp->bdaddr, addr);
	cp->pscan_rep_mode = pscan;
	cp->reserved = 0x00; /* reserved, should be set to 0x00 */
	cp->clock_offset = offset;

	return bt_hci_cmd_send_sync(BT_HCI_OP_REMOTE_NAME_REQUEST, buf, NULL);
}

#define EIR_SHORT_NAME    0x08
#define EIR_COMPLETE_NAME 0x09

static bool eir_has_name(const uint8_t *eir)
{
	int len = 240;

	while (len) {
		if (len < 2) {
			break;
		}

		/* Look for early termination */
		if (!eir[0]) {
			break;
		}

		/* Check if field length is correct */
		if (eir[0] > len - 1) {
			break;
		}

		switch (eir[1]) {
		case EIR_SHORT_NAME:
		case EIR_COMPLETE_NAME:
			if (eir[0] > 1) {
				return true;
			}
			break;
		default:
			break;
		}

		/* Parse next AD Structure */
		len -= eir[0] + 1;
		eir += eir[0] + 1;
	}

	return false;
}

void bt_br_discovery_reset(void)
{
	discovery_results = NULL;
	discovery_results_size = 0;
	discovery_results_count = 0;
}

static bool check_request_name(void)
{
	int i;
	bool resolving_names = false;

	for (i = 0; i < discovery_results_count; i++) {
		struct bt_br_discovery_priv *priv;

		priv = &discovery_results[i]._priv;

		if (eir_has_name(discovery_results[i].eir)) {
			continue;
		}

		if (priv->resolve_state != RESOLVE_REMOTE_NAME_PENDING) {
			continue;
		}

		if (request_name(&discovery_results[i].addr, priv->pscan_rep_mode,
				 priv->clock_offset)) {
			priv->resolve_state = RESOLVE_REMOTE_NAME_RESOLVED;
			continue;
		}

		priv->resolve_state = RESOLVE_REMOTE_NAME_RESOLVING;
		resolving_names = true;
		break;
	}

	return resolving_names;
}

static void report_discovery_results(void)
{
	int i;
	struct bt_br_discovery_cb *listener, *next;

	for (i = 0; i < discovery_results_count; i++) {
		struct bt_br_discovery_priv *priv;

		priv = &discovery_results[i]._priv;
		priv->resolve_state = RESOLVE_REMOTE_NAME_PENDING;
	}

	if (check_request_name()) {
		return;
	}

	atomic_clear_bit(bt_dev.flags, BT_DEV_INQUIRY);

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&discovery_cbs, listener, next, node) {
		if (listener->timeout) {
			listener->timeout(discovery_results, discovery_results_count);
		}
	}

	bt_br_discovery_reset();
}

void bt_hci_inquiry_complete(struct net_buf *buf)
{
	struct bt_hci_evt_inquiry_complete *evt = (void *)buf->data;

	if (evt->status) {
		LOG_ERR("Failed to complete inquiry");
	}

	report_discovery_results();
}

static struct bt_br_discovery_result *get_result_slot(const bt_addr_t *addr, int8_t rssi)
{
	struct bt_br_discovery_result *result = NULL;
	size_t i;

	/* check if already present in results */
	for (i = 0; i < discovery_results_count; i++) {
		if (bt_addr_eq(addr, &discovery_results[i].addr)) {
			return &discovery_results[i];
		}
	}

	/* Pick a new slot (if available) */
	if (discovery_results_count < discovery_results_size) {
		bt_addr_copy(&discovery_results[discovery_results_count].addr, addr);
		return &discovery_results[discovery_results_count++];
	}

	/* ignore if invalid RSSI */
	if (rssi == RSSI_INVALID) {
		return NULL;
	}

	/*
	 * Pick slot with smallest RSSI that is smaller then passed RSSI
	 * TODO handle TX if present
	 */
	for (i = 0; i < discovery_results_size; i++) {
		if (discovery_results[i].rssi > rssi) {
			continue;
		}

		if (!result || result->rssi > discovery_results[i].rssi) {
			result = &discovery_results[i];
		}
	}

	if (result) {
		LOG_DBG("Reusing slot (old %s rssi %d dBm)", bt_addr_str(&result->addr),
			result->rssi);

		bt_addr_copy(&result->addr, addr);
	}

	return result;
}

void bt_hci_inquiry_result_with_rssi(struct net_buf *buf)
{
	uint8_t num_reports = net_buf_pull_u8(buf);

	if (!atomic_test_bit(bt_dev.flags, BT_DEV_INQUIRY)) {
		return;
	}

	LOG_DBG("number of results: %u", num_reports);

	while (num_reports--) {
		struct bt_hci_evt_inquiry_result_with_rssi *evt;
		struct bt_br_discovery_result *result;
		struct bt_br_discovery_priv *priv;
		struct bt_br_discovery_cb *listener, *next;

		if (buf->len < sizeof(*evt)) {
			LOG_ERR("Unexpected end to buffer");
			return;
		}

		evt = net_buf_pull_mem(buf, sizeof(*evt));
		LOG_DBG("%s rssi %d dBm", bt_addr_str(&evt->addr), evt->rssi);

		result = get_result_slot(&evt->addr, evt->rssi);
		if (!result) {
			return;
		}

		priv = &result->_priv;
		priv->pscan_rep_mode = evt->pscan_rep_mode;
		priv->clock_offset = evt->clock_offset;

		memcpy(result->cod, evt->cod, 3);
		result->rssi = evt->rssi;

		/* we could reuse slot so make sure EIR is cleared */
		(void)memset(result->eir, 0, sizeof(result->eir));

		SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&discovery_cbs, listener, next, node) {
			if (listener->recv) {
				listener->recv(result);
			}
		}
	}
}

void bt_hci_extended_inquiry_result(struct net_buf *buf)
{
	struct bt_hci_evt_extended_inquiry_result *evt = (void *)buf->data;
	struct bt_br_discovery_result *result;
	struct bt_br_discovery_priv *priv;
	struct bt_br_discovery_cb *listener, *next;

	if (!atomic_test_bit(bt_dev.flags, BT_DEV_INQUIRY)) {
		return;
	}

	LOG_DBG("%s rssi %d dBm", bt_addr_str(&evt->addr), evt->rssi);

	result = get_result_slot(&evt->addr, evt->rssi);
	if (!result) {
		return;
	}

	priv = &result->_priv;
	priv->pscan_rep_mode = evt->pscan_rep_mode;
	priv->clock_offset = evt->clock_offset;

	result->rssi = evt->rssi;
	memcpy(result->cod, evt->cod, 3);
	memcpy(result->eir, evt->eir, sizeof(result->eir));

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&discovery_cbs, listener, next, node) {
		if (listener->recv) {
			listener->recv(result);
		}
	}
}

void bt_hci_remote_name_request_complete(struct net_buf *buf)
{
	struct bt_hci_evt_remote_name_req_complete *evt = (void *)buf->data;
	struct bt_br_discovery_result *result;
	struct bt_br_discovery_priv *priv;
	int eir_len = 240;
	uint8_t *eir;
	struct bt_br_discovery_cb *listener, *next;

	result = get_result_slot(&evt->bdaddr, RSSI_INVALID);
	if (!result) {
		return;
	}

	priv = &result->_priv;
	priv->resolve_state = RESOLVE_REMOTE_NAME_RESOLVED;

	if (evt->status) {
		goto check_names;
	}

	eir = result->eir;

	while (eir_len) {
		if (eir_len < 2) {
			break;
		}

		/* Look for early termination */
		if (!eir[0]) {
			size_t name_len;

			eir_len -= 2;

			/* name is null terminated */
			name_len = strlen((const char *)evt->name);

			if (name_len > eir_len) {
				eir[0] = eir_len + 1;
				eir[1] = EIR_SHORT_NAME;
			} else {
				eir[0] = name_len + 1;
				eir[1] = EIR_SHORT_NAME;
			}

			memcpy(&eir[2], evt->name, eir[0] - 1);

			break;
		}

		/* Check if field length is correct */
		if (eir[0] > eir_len - 1) {
			break;
		}

		/* next EIR Structure */
		eir_len -= eir[0] + 1;
		eir += eir[0] + 1;
	}

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&discovery_cbs, listener, next, node) {
		if (listener->recv) {
			listener->recv(result);
		}
	}

check_names:
	/* if still need to request name */
	if (check_request_name()) {
		return;
	}

	/* all names resolved, report discovery results */
	atomic_clear_bit(bt_dev.flags, BT_DEV_INQUIRY);

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&discovery_cbs, listener, next, node) {
		if (listener->timeout) {
			listener->timeout(discovery_results, discovery_results_count);
		}
	}
}

void bt_hci_read_remote_features_complete(struct net_buf *buf)
{
	struct bt_hci_evt_remote_features *evt = (void *)buf->data;
	uint16_t handle = sys_le16_to_cpu(evt->handle);
	struct bt_hci_cp_read_remote_ext_features *cp;
	struct bt_conn *conn;

	LOG_DBG("status 0x%02x handle %u", evt->status, handle);

	conn = bt_conn_lookup_handle(handle, BT_CONN_TYPE_BR);
	if (!conn) {
		LOG_ERR("Can't find conn for handle %u", handle);
		return;
	}

	if (evt->status) {
		goto done;
	}

	memcpy(conn->br.features[0], evt->features, sizeof(evt->features));

	if (!BT_FEAT_EXT_FEATURES(conn->br.features)) {
		goto done;
	}

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		goto done;
	}

	/* Read remote host features (page 1) */
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = evt->handle;
	cp->page = 0x01;

	bt_hci_cmd_send_sync(BT_HCI_OP_READ_REMOTE_EXT_FEATURES, buf, NULL);

done:
	bt_conn_unref(conn);
}

void bt_hci_read_remote_ext_features_complete(struct net_buf *buf)
{
	struct bt_hci_evt_remote_ext_features *evt = (void *)buf->data;
	uint16_t handle = sys_le16_to_cpu(evt->handle);
	struct bt_conn *conn;

	LOG_DBG("status 0x%02x handle %u", evt->status, handle);

	conn = bt_conn_lookup_handle(handle, BT_CONN_TYPE_BR);
	if (!conn) {
		LOG_ERR("Can't find conn for handle %u", handle);
		return;
	}

	if (!evt->status && evt->page == 0x01) {
		memcpy(conn->br.features[1], evt->features, sizeof(conn->br.features[1]));
	}

	bt_conn_unref(conn);
}

void bt_hci_role_change(struct net_buf *buf)
{
	struct bt_hci_evt_role_change *evt = (void *)buf->data;
	struct bt_conn *conn;

	LOG_DBG("status 0x%02x role %u addr %s", evt->status, evt->role, bt_addr_str(&evt->bdaddr));

	conn = bt_conn_lookup_addr_br(&evt->bdaddr);
	if (!conn) {
		LOG_ERR("Can't find conn for %s", bt_addr_str(&evt->bdaddr));
		return;
	}

	if (evt->status == 0) {
		if (evt->role == BT_HCI_ROLE_PERIPHERAL) {
			conn->role = BT_CONN_ROLE_PERIPHERAL;
		} else {
			conn->role = BT_CONN_ROLE_CENTRAL;
		}
	}

	bt_conn_role_changed(conn, evt->status);

	bt_conn_unref(conn);
}

static int read_ext_features(void)
{
	int i;

	/* Read Local Supported Extended Features */
	for (i = 1; i < LMP_FEAT_PAGES_COUNT; i++) {
		struct bt_hci_cp_read_local_ext_features *cp;
		struct bt_hci_rp_read_local_ext_features *rp;
		struct net_buf *buf, *rsp;
		int err;

		buf = bt_hci_cmd_alloc(K_FOREVER);
		if (!buf) {
			return -ENOBUFS;
		}

		cp = net_buf_add(buf, sizeof(*cp));
		cp->page = i;

		err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_LOCAL_EXT_FEATURES, buf, &rsp);
		if (err) {
			return err;
		}

		rp = (void *)rsp->data;

		memcpy(&bt_dev.features[i], rp->ext_features, sizeof(bt_dev.features[i]));

		if (rp->max_page <= i) {
			net_buf_unref(rsp);
			break;
		}

		net_buf_unref(rsp);
	}

	return 0;
}

void device_supported_pkt_type(void)
{
	/* Device supported features and sco packet types */
	if (BT_FEAT_LMP_SCO_CAPABLE(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_SCO_HV1);
	}

	if (BT_FEAT_HV2_PKT(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_SCO_HV2);
	}

	if (BT_FEAT_HV3_PKT(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_SCO_HV3);
	}

	if (BT_FEAT_LMP_ESCO_CAPABLE(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_ESCO_EV3);
	}

	if (BT_FEAT_EV4_PKT(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_ESCO_EV4);
	}

	if (BT_FEAT_EV5_PKT(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_ESCO_EV5);
	}

	if (BT_FEAT_2EV3_PKT(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_ESCO_2EV3);
	}

	if (BT_FEAT_3EV3_PKT(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_ESCO_3EV3);
	}

	if (BT_FEAT_3SLOT_PKT(bt_dev.features)) {
		bt_dev.br.esco_pkt_type |= (HCI_PKT_TYPE_ESCO_2EV5 | HCI_PKT_TYPE_ESCO_3EV5);
	}
}

static void read_buffer_size_complete(struct net_buf *buf)
{
	struct bt_hci_rp_read_buffer_size *rp = (void *)buf->data;
	uint16_t pkts;

	LOG_DBG("status 0x%02x", rp->status);

	bt_dev.br.mtu = sys_le16_to_cpu(rp->acl_max_len);
	pkts = sys_le16_to_cpu(rp->acl_max_num);

	LOG_DBG("ACL BR/EDR buffers: pkts %u mtu %u", pkts, bt_dev.br.mtu);

	k_sem_init(&bt_dev.br.pkts, pkts, pkts);
}

int bt_br_init(void)
{
	struct net_buf *buf;
	struct bt_hci_cp_write_ssp_mode *ssp_cp;
	struct bt_hci_cp_write_inquiry_mode *inq_cp;
	struct bt_hci_write_local_name *name_cp;
	struct bt_hci_rp_read_default_link_policy_settings *rp;
	struct net_buf *rsp;
	int err;
	uint16_t default_link_policy_settings;

	/* Read extended local features */
	if (BT_FEAT_EXT_FEATURES(bt_dev.features)) {
		err = read_ext_features();
		if (err) {
			return err;
		}
	}

	/* Add local supported packet types to bt_dev */
	device_supported_pkt_type();

	/* Get BR/EDR buffer size */
	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_BUFFER_SIZE, NULL, &buf);
	if (err) {
		return err;
	}

	read_buffer_size_complete(buf);
	net_buf_unref(buf);

	/* Set SSP mode */
	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	ssp_cp = net_buf_add(buf, sizeof(*ssp_cp));
	ssp_cp->mode = 0x01;
	err = bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_SSP_MODE, buf, NULL);
	if (err) {
		return err;
	}

	/* Enable Inquiry results with RSSI or extended Inquiry */
	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	inq_cp = net_buf_add(buf, sizeof(*inq_cp));
	inq_cp->mode = 0x02;
	err = bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_INQUIRY_MODE, buf, NULL);
	if (err) {
		return err;
	}

	/* Set local name */
	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	name_cp = net_buf_add(buf, sizeof(*name_cp));
	strncpy((char *)name_cp->local_name, CONFIG_BT_DEVICE_NAME, sizeof(name_cp->local_name));

	err = bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_LOCAL_NAME, buf, NULL);
	if (err) {
		return err;
	}

	/* Set Class of device */
	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	net_buf_add_le24(buf, CONFIG_BT_COD);

	err = bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_CLASS_OF_DEVICE, buf, NULL);
	if (err) {
		return err;
	}

	/* Set page timeout*/
	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	net_buf_add_le16(buf, CONFIG_BT_PAGE_TIMEOUT);

	err = bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_PAGE_TIMEOUT, buf, NULL);
	if (err) {
		return err;
	}

	/* Enable BR/EDR SC if supported */
	if (BT_FEAT_SC(bt_dev.features)) {
		struct bt_hci_cp_write_sc_host_supp *sc_cp;

		buf = bt_hci_cmd_alloc(K_FOREVER);
		if (!buf) {
			return -ENOBUFS;
		}

		sc_cp = net_buf_add(buf, sizeof(*sc_cp));
		sc_cp->sc_support = 0x01;

		err = bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_SC_HOST_SUPP, buf, NULL);
		if (err) {
			return err;
		}
	}

	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_DEFAULT_LINK_POLICY_SETTINGS, NULL, &rsp);
	if (err) {
		return err;
	}

	rp = (void *)rsp->data;
	default_link_policy_settings = rp->default_link_policy_settings;

	bool should_enable = IS_ENABLED(CONFIG_BT_DEFAULT_ROLE_SWITCH_ENABLE);
	bool is_enabled = (default_link_policy_settings &
			   BT_HCI_LINK_POLICY_SETTINGS_ENABLE_ROLE_SWITCH);

	/* Enable/Disable the default role switch */
	if (should_enable != is_enabled) {
		struct bt_hci_cp_write_default_link_policy_settings *policy_cp;

		default_link_policy_settings ^= BT_HCI_LINK_POLICY_SETTINGS_ENABLE_ROLE_SWITCH;

		buf = bt_hci_cmd_alloc(K_FOREVER);
		if (!buf) {
			return -ENOBUFS;
		}

		policy_cp = net_buf_add(buf, sizeof(*policy_cp));
		policy_cp->default_link_policy_settings = default_link_policy_settings;

		err = bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_DEFAULT_LINK_POLICY_SETTINGS, buf, NULL);
		if (err) {
			return err;
		}
	}

	return 0;
}

static int br_start_inquiry(const struct bt_br_discovery_param *param)
{
	const uint8_t iac[3] = {0x33, 0x8b, 0x9e};
	struct bt_hci_op_inquiry *cp;
	struct net_buf *buf;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	cp = net_buf_add(buf, sizeof(*cp));

	cp->length = param->length;
	cp->num_rsp = 0xff; /* we limit discovery only by time */

	memcpy(cp->lap, iac, 3);
	if (param->limited) {
		cp->lap[0] = 0x00;
	}

	return bt_hci_cmd_send_sync(BT_HCI_OP_INQUIRY, buf, NULL);
}

static bool valid_br_discov_param(const struct bt_br_discovery_param *param, size_t num_results)
{
	if (!num_results || num_results > 255) {
		return false;
	}

	if (!param->length || param->length > 0x30) {
		return false;
	}

	return true;
}

int bt_br_discovery_start(const struct bt_br_discovery_param *param,
			  struct bt_br_discovery_result *results, size_t cnt)
{
	int err;

	LOG_DBG("");

	if (!valid_br_discov_param(param, cnt)) {
		return -EINVAL;
	}

	if (atomic_test_bit(bt_dev.flags, BT_DEV_INQUIRY)) {
		return -EALREADY;
	}

	err = br_start_inquiry(param);
	if (err) {
		return err;
	}

	atomic_set_bit(bt_dev.flags, BT_DEV_INQUIRY);

	(void)memset(results, 0, sizeof(*results) * cnt);

	discovery_results = results;
	discovery_results_size = cnt;
	discovery_results_count = 0;

	return 0;
}

int bt_br_discovery_stop(void)
{
	int err;
	int i;

	LOG_DBG("");

	if (!atomic_test_bit(bt_dev.flags, BT_DEV_INQUIRY)) {
		return -EALREADY;
	}

	err = bt_hci_cmd_send_sync(BT_HCI_OP_INQUIRY_CANCEL, NULL, NULL);
	if (err) {
		return err;
	}

	for (i = 0; i < discovery_results_count; i++) {
		struct bt_br_discovery_priv *priv;
		struct bt_hci_cp_remote_name_cancel *cp;
		struct net_buf *buf;

		priv = &discovery_results[i]._priv;

		if (priv->resolve_state != RESOLVE_REMOTE_NAME_RESOLVING) {
			continue;
		}

		buf = bt_hci_cmd_alloc(K_FOREVER);
		if (!buf) {
			continue;
		}

		cp = net_buf_add(buf, sizeof(*cp));
		bt_addr_copy(&cp->bdaddr, &discovery_results[i].addr);

		bt_hci_cmd_send_sync(BT_HCI_OP_REMOTE_NAME_CANCEL, buf, NULL);
	}

	atomic_clear_bit(bt_dev.flags, BT_DEV_INQUIRY);

	discovery_results = NULL;
	discovery_results_size = 0;
	discovery_results_count = 0;

	return 0;
}

void bt_br_discovery_cb_register(struct bt_br_discovery_cb *cb)
{
	sys_slist_append(&discovery_cbs, &cb->node);
}

void bt_br_discovery_cb_unregister(struct bt_br_discovery_cb *cb)
{
	sys_slist_find_and_remove(&discovery_cbs, &cb->node);
}

static int write_scan_enable(uint8_t scan)
{
	struct net_buf *buf;
	int err;

	LOG_DBG("type %u", scan);

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	net_buf_add_u8(buf, scan);
	err = bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_SCAN_ENABLE, buf, NULL);
	if (err) {
		return err;
	}

	atomic_set_bit_to(bt_dev.flags, BT_DEV_ISCAN, (scan & BT_BREDR_SCAN_INQUIRY));
	atomic_set_bit_to(bt_dev.flags, BT_DEV_PSCAN, (scan & BT_BREDR_SCAN_PAGE));

	return 0;
}

int bt_br_set_connectable(bool enable)
{
	if (enable) {
		if (atomic_test_bit(bt_dev.flags, BT_DEV_PSCAN)) {
			return -EALREADY;
		} else {
			return write_scan_enable(BT_BREDR_SCAN_PAGE);
		}
	} else {
		if (!atomic_test_bit(bt_dev.flags, BT_DEV_PSCAN)) {
			return -EALREADY;
		} else {
			return write_scan_enable(BT_BREDR_SCAN_DISABLED);
		}
	}
}

#define BT_LIAC 0x9e8b00
#define BT_GIAC 0x9e8b33

static int bt_br_write_current_iac_lap(bool limited)
{
	struct bt_hci_cp_write_current_iac_lap *iac_lap;
	struct net_buf *buf;
	uint8_t param_len;
	uint8_t num_current_iac = limited ? 2 : 1;

	LOG_DBG("limited discoverable mode? %s", limited ? "Yes" : "No");

	param_len = sizeof(*iac_lap) + (num_current_iac * sizeof(struct bt_hci_iac_lap));

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	iac_lap = net_buf_add(buf, param_len);
	iac_lap->num_current_iac = num_current_iac;
	sys_put_le24(BT_GIAC, iac_lap->lap[0].iac);
	if (num_current_iac > 1) {
		sys_put_le24(BT_LIAC, iac_lap->lap[1].iac);
	}

	return bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_CURRENT_IAC_LAP, buf, NULL);
}

#define BT_COD_MAJOR_SVC_CLASS_LIMITED_DISCOVER BIT(13)

static int bt_br_read_cod(uint32_t *cod)
{
	struct net_buf *rsp;
	struct bt_hci_rp_read_class_of_device *rp;
	int err;

	LOG_DBG("Read COD");

	/* Read Class of device */
	err = bt_hci_cmd_send_sync(BT_HCI_OP_READ_CLASS_OF_DEVICE, NULL, &rsp);
	if (err) {
		LOG_WRN("Fail to read COD");
		return err;
	}

	if (!rsp || (rsp->len < sizeof(*rp))) {
		LOG_WRN("Invalid response");
		return -EIO;
	}

	rp = (void *)rsp->data;
	*cod = sys_get_le24(rp->class_of_device);

	LOG_DBG("Current COD %06x", *cod);

	net_buf_unref(rsp);

	return 0;
}

static int bt_br_write_cod(uint32_t cod)
{
	struct net_buf *buf;

	/* Set Class of device */
	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		return -ENOBUFS;
	}

	net_buf_add_le24(buf, cod);

	return bt_hci_cmd_send_sync(BT_HCI_OP_WRITE_CLASS_OF_DEVICE, buf, NULL);
}

static int bt_br_update_cod(bool limited)
{
	int err;
	uint32_t cod;

	LOG_DBG("Update COD");

	err = bt_br_read_cod(&cod);
	if (err) {
		return err;
	}

	if (limited) {
		cod |= BT_COD_MAJOR_SVC_CLASS_LIMITED_DISCOVER;
	} else {
		cod &= ~BT_COD_MAJOR_SVC_CLASS_LIMITED_DISCOVER;
	}

	err = bt_br_write_cod(cod);
	return err;
}

static void bt_br_limited_discoverable_timeout_handler(struct k_work *work)
{
	int err;

	if (!atomic_test_bit(bt_dev.flags, BT_DEV_LIMITED_DISCOVERABLE_MODE)) {
		LOG_INF("Limited discoverable mode has been disabled");
		return;
	}

	err = bt_br_set_discoverable(false, false);
	if (err) {
		LOG_WRN("Disable discoverable failure (err %d)", err);
	}
}

/* Work used for limited discoverable mode time span */
static K_WORK_DELAYABLE_DEFINE(bt_br_limited_discoverable_timeout,
			       bt_br_limited_discoverable_timeout_handler);

int bt_br_set_discoverable(bool enable, bool limited)
{
	int err;

	if (enable) {
		if (atomic_test_bit(bt_dev.flags, BT_DEV_ISCAN)) {
			return -EALREADY;
		}

		if (!atomic_test_bit(bt_dev.flags, BT_DEV_PSCAN)) {
			return -EPERM;
		}

		err = bt_br_write_current_iac_lap(limited);
		if (err) {
			return err;
		}

		err = bt_br_update_cod(limited);
		if (err) {
			return err;
		}

		err = write_scan_enable(BT_BREDR_SCAN_INQUIRY | BT_BREDR_SCAN_PAGE);
		if (!err && (limited == true)) {
			atomic_set_bit(bt_dev.flags, BT_DEV_LIMITED_DISCOVERABLE_MODE);
			k_work_reschedule(&bt_br_limited_discoverable_timeout,
					  K_SECONDS(CONFIG_BT_LIMITED_DISCOVERABLE_DURATION));
		}
		return err;
	}

	if (!atomic_test_bit(bt_dev.flags, BT_DEV_ISCAN)) {
		return -EALREADY;
	}

	err = write_scan_enable(BT_BREDR_SCAN_PAGE);
	if (err) {
		return err;
	}

	if (atomic_test_bit(bt_dev.flags, BT_DEV_LIMITED_DISCOVERABLE_MODE)) {
		err = bt_br_write_current_iac_lap(false);
		if (err) {
			return err;
		}

		err = bt_br_update_cod(false);
		if (err) {
			return err;
		}

		atomic_clear_bit(bt_dev.flags, BT_DEV_LIMITED_DISCOVERABLE_MODE);
		k_work_cancel_delayable(&bt_br_limited_discoverable_timeout);
	}

	return 0;
}

bool bt_br_bond_exists(const bt_addr_t *addr)
{
	struct bt_keys_link_key *key = bt_keys_find_link_key(addr);

	/* if there are any keys stored then device is bonded */
	return key != NULL;
}

static void unpair(const bt_addr_t *addr)
{
	struct bt_conn *conn = bt_conn_lookup_addr_br(addr);
	struct bt_conn_auth_info_cb *listener, *next;

	if (conn) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(conn);
	}

	bt_keys_link_key_clear_addr(addr);

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&bt_auth_info_cbs, listener,
					  next, node) {
		if (listener->br_bond_deleted) {
			listener->br_bond_deleted(addr);
		}
	}
}

static void bt_br_unpair_remote(const struct bt_br_bond_info *info, void *data)
{
	ARG_UNUSED(data);

	unpair(&info->addr);
}

int bt_br_unpair(const bt_addr_t *addr)
{
	if (!addr || bt_addr_eq(addr, BT_ADDR_ANY)) {
		bt_br_foreach_bond(bt_br_unpair_remote, NULL);
	} else {
		unpair(addr);
	}

	return 0;
}
