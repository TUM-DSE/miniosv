/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2017 Intel Corporation
 */

#include "iavf.h"
#include "iavf_ethdev.hh"
#include "iavf_rxtx.h"

#define MAX_TRY_TIMES 2000
#define ASQ_DELAY_MS  1

static uint32_t
iavf_convert_link_speed(enum virtchnl_link_speed virt_link_speed)
{
	uint32_t speed;

	switch (virt_link_speed) {
	case VIRTCHNL_LINK_SPEED_100MB:
		speed = 100;
		break;
	case VIRTCHNL_LINK_SPEED_1GB:
		speed = 1000;
		break;
	case VIRTCHNL_LINK_SPEED_10GB:
		speed = 10000;
		break;
	case VIRTCHNL_LINK_SPEED_40GB:
		speed = 40000;
		break;
	case VIRTCHNL_LINK_SPEED_20GB:
		speed = 20000;
		break;
	case VIRTCHNL_LINK_SPEED_25GB:
		speed = 25000;
		break;
	case VIRTCHNL_LINK_SPEED_2_5GB:
		speed = 2500;
		break;
	case VIRTCHNL_LINK_SPEED_5GB:
		speed = 5000;
		break;
	default:
		speed = 0;
		break;
	}

	return speed;
}

/*
 * iavf_handle_link_change_event: common handler for VIRTCHNL link change events
 *
 * @dev: pointer to rte_eth_dev for this VF
 * @vpe: pointer to the virtchnl_pf_event payload received from the PF
 *
 * Handle PF link-change event: decode adv/legacy link info, update VF
 * link state, sync no-poll/watchdog behavior & notify app via LSC event.
 */
static void
iavf_handle_link_change_event(struct rte_eth_dev *dev,
			      struct virtchnl_pf_event *vpe)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = &adapter->vf;
	bool adv_link_speed;

	adv_link_speed = (vf->vf_res != NULL) &&
		((vf->vf_res->vf_cap_flags & VIRTCHNL_VF_CAP_ADV_LINK_SPEED) != 0);

	if (adv_link_speed) {
		vf->link_up = vpe->event_data.link_event_adv.link_status;
		vf->link_speed = vpe->event_data.link_event_adv.link_speed;
	} else {
		enum virtchnl_link_speed speed;

		vf->link_up = vpe->event_data.link_event.link_status;
		speed = vpe->event_data.link_event.link_speed;
		vf->link_speed = iavf_convert_link_speed(speed);
	}

	iavf_dev_link_update(dev, 0);

	PMD_DRV_LOG(INFO, "Link status update:%s",
		vf->link_up ? "up" : "down");
}

/* Read data in admin queue to get msg from pf driver */
static enum iavf_aq_result
iavf_read_msg_from_pf(struct iavf_adapter *adapter, uint16_t buf_len,
		     uint8_t *buf)
{
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct iavf_arq_event_info event;
	enum iavf_aq_result result = IAVF_MSG_NON;
	enum virtchnl_ops opcode;
	int ret;

	event.buf_len = buf_len;
	event.msg_buf = buf;
	ret = iavf_clean_arq_element(hw, &event, NULL);
	/* Can't read any msg from adminQ */
	if (ret) {
		PMD_DRV_LOG(DEBUG, "Can't read msg from AQ");
		if (ret != IAVF_ERR_ADMIN_QUEUE_NO_WORK)
			result = IAVF_MSG_ERR;
		return result;
	}

	opcode = (enum virtchnl_ops)rte_le_to_cpu_32(event.desc.cookie_high);
	vf->cmd_retval = (enum virtchnl_status_code)rte_le_to_cpu_32(
			event.desc.cookie_low);

	PMD_DRV_LOG(DEBUG, "AQ from pf carries opcode %u, retval %d",
		    opcode, vf->cmd_retval);

	if (opcode == VIRTCHNL_OP_EVENT) {
		struct virtchnl_pf_event *vpe =
			(struct virtchnl_pf_event *)event.msg_buf;

		result = IAVF_MSG_SYS;
		switch (vpe->event) {
		case VIRTCHNL_EVENT_LINK_CHANGE:
			iavf_handle_link_change_event(vf->eth_dev, vpe);
			break;
		case VIRTCHNL_EVENT_RESET_IMPENDING:
			/*
			 * Force link down on impending reset to drop
			 * the cached link-up state; a fresh LSC up
			 * event will be re-issued by the PF once the
			 * VF is reinitialised.
			 */
			vf->link_up = false;
			if (!vf->vf_reset) {
				vf->vf_reset = true;
				iavf_set_no_poll(adapter, false);
				PMD_DRV_LOG(WARNING, "PF is resetting the VF");
			}
			PMD_DRV_LOG(DEBUG, "VF is resetting");
			break;
		case VIRTCHNL_EVENT_PF_DRIVER_CLOSE:
			vf->dev_closed = true;
			PMD_DRV_LOG(INFO, "PF driver closed");
			break;
		default:
			PMD_DRV_LOG(ERR, "%s: Unknown event %d from pf",
					__func__, vpe->event);
		}
	}  else {
		/* async reply msg on command issued by vf previously */
		result = IAVF_MSG_CMD;
		if (opcode != vf->pend_cmd) {
			if (opcode == VIRTCHNL_OP_UNKNOWN)
				PMD_DRV_LOG(DEBUG,
					    "Spurious msg with opcode 0, pending cmd %u",
					    vf->pend_cmd);
			else
				PMD_DRV_LOG(WARNING,
					    "command mismatch, expect %u, get %u",
					    vf->pend_cmd, opcode);

			result = IAVF_MSG_ERR;
		}
	}

	return result;
}

static int
iavf_set_pending_cmd(struct iavf_info *vf, enum virtchnl_ops ops,
		    uint32_t resp_count)
{
	enum virtchnl_ops op_unk = VIRTCHNL_OP_UNKNOWN;
	int ret = rte_atomic_compare_exchange_strong_explicit(&vf->pend_cmd,
			&op_unk, ops, rte_memory_order_acquire,
			rte_memory_order_acquire);

	if (ret == 0) {
		PMD_DRV_LOG(ERR, "There is incomplete cmd %d", vf->pend_cmd);
		return -1;
	}

	rte_atomic_store_explicit(&vf->pend_cmd_count, resp_count,
			rte_memory_order_relaxed);

	return 0;
}

static inline void
iavf_clear_pending_cmd(struct iavf_info *vf)
{
	rte_wmb();
	vf->pend_cmd = VIRTCHNL_OP_UNKNOWN;
	vf->cmd_retval = VIRTCHNL_STATUS_SUCCESS;
}

static inline void
iavf_notify_pending_cmd(struct iavf_info *vf, int msg_ret)
{
	vf->cmd_retval = msg_ret;
	rte_wmb();
	vf->pend_cmd = VIRTCHNL_OP_UNKNOWN;
}

static int
iavf_get_cmd_resp_count(enum virtchnl_ops op)
{
	switch (op) {
	case VIRTCHNL_OP_RESET_VF:
	case VIRTCHNL_OP_REQUEST_QUEUES:
		/* These commands trigger reset and are not waited for */
		return 0;
	case VIRTCHNL_OP_INLINE_IPSEC_CRYPTO:
		/* IPsec crypto commands generate two responses */
		return 2;
	default:
		/* All other commands generate one response */
		return 1;
	}
}

static int
iavf_op_needs_poll(struct iavf_info *vf)
{
	/* Poll if interrupts disabled or we are in interrupt thread */
	return !vf->aq_intr_enabled || iavf_in_mgmt_thread(vf->eth_dev);
}

static int
iavf_wait_for_msg(struct iavf_adapter *adapter, struct iavf_cmd_info *args,
		bool poll)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	int err = 0;
	int i = 0;

	if (args->out_buffer == NULL) {
		PMD_DRV_LOG(ERR, "Invalid buffer for cmd %d response", args->ops);
		return -EINVAL;
	}

	/* Wait for command completion */
	do {
		if (poll) {
			enum iavf_aq_result result;
			result = iavf_read_msg_from_pf(adapter, args->out_size,
					args->out_buffer);
			if (result == IAVF_MSG_CMD)
				break;
		} else {
			/* check if interrupt thread has erased pending cmd */
			if (vf->pend_cmd == VIRTCHNL_OP_UNKNOWN)
				break;
		}
		iavf_msec_delay(ASQ_DELAY_MS);
	} while (i++ < MAX_TRY_TIMES);

	if (i >= MAX_TRY_TIMES) {
		PMD_DRV_LOG(ERR, "No response for cmd %d", args->ops);
		err = -EIO;
	} else if (vf->cmd_retval == VIRTCHNL_STATUS_ERR_NOT_SUPPORTED) {
		PMD_DRV_LOG(ERR, "Cmd %d not supported", args->ops);
		err = -ENOTSUP;
	} else if (vf->cmd_retval != VIRTCHNL_STATUS_SUCCESS) {
		PMD_DRV_LOG(ERR, "Return failure %d for cmd %d",
				vf->cmd_retval, args->ops);
		err = -EINVAL;
	}

	return err;
}

static int
iavf_execute_vf_cmd(struct iavf_adapter *adapter, struct iavf_cmd_info *args)
{
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	enum iavf_status status;
	uint32_t resp_count;
	bool poll = iavf_op_needs_poll(vf);
	int err;

	if (vf->vf_reset)
		return -EIO;

	resp_count = iavf_get_cmd_resp_count(args->ops);

	/*
	 * For some commands, we are not waiting for responses because they
	 * produce a reset event. However, we still need to set pending command
	 * to avoid sending commands while another one is already in progress.
	 */
	if (iavf_set_pending_cmd(vf, args->ops, RTE_MAX(1U, resp_count)) != 0)
		return -1;

	status = iavf_aq_send_msg_to_pf(hw, args->ops, IAVF_SUCCESS,
			args->in_args, args->in_args_size, NULL);
	if (status != IAVF_SUCCESS) {
		PMD_DRV_LOG(ERR, "fail to send cmd %d", args->ops);
		err = (int)status;
		goto clear_cmd;
	} else if (resp_count == 0) {
		/* we're not waiting on responses */
		err = 0;
		goto clear_cmd;
	}

	err = iavf_wait_for_msg(adapter, args, poll);

	/*
	 * messages received through interrupts store their response in global
	 * buffer, so we need to copy it if we received the message.
	 */
	if (!poll && err == 0)
		memcpy(args->out_buffer, vf->aq_resp, args->out_size);

	/* in interrupt success case, pending cmd is cleared by intr thread */
	if (err != 0 || poll)
		goto clear_cmd;

	return err;
clear_cmd:
	iavf_clear_pending_cmd(vf);
	return err;
}

static int
iavf_execute_vf_cmd_safe(struct iavf_adapter *adapter,
	struct iavf_cmd_info *args)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	int ret;
	int is_intr_thread = iavf_in_mgmt_thread(vf->eth_dev);

	if (is_intr_thread) {
		if (!rte_spinlock_trylock(&vf->aq_lock))
			return -EIO;
	} else {
		rte_spinlock_lock(&vf->aq_lock);
	}
	ret = iavf_execute_vf_cmd(adapter, args);
	rte_spinlock_unlock(&vf->aq_lock);

	return ret;
}

static void
iavf_handle_pf_event_msg(struct rte_eth_dev *dev, uint8_t *msg,
			uint16_t msglen)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf = &adapter->vf;
	struct virtchnl_pf_event *pf_msg =
			(struct virtchnl_pf_event *)msg;

	if (adapter->closed) {
		PMD_DRV_LOG(DEBUG, "Port closed");
		return;
	}

	if (msglen < sizeof(struct virtchnl_pf_event)) {
		PMD_DRV_LOG(DEBUG, "Error event");
		return;
	}
	switch (pf_msg->event) {
	case VIRTCHNL_EVENT_RESET_IMPENDING:
		PMD_DRV_LOG(DEBUG, "VIRTCHNL_EVENT_RESET_IMPENDING event");
		vf->link_up = false;
		if (!vf->vf_reset) {
			vf->vf_reset = true;
			iavf_set_no_poll(adapter, false);
			PMD_DRV_LOG(WARNING, "PF is resetting the VF");
		}
		break;
	case VIRTCHNL_EVENT_LINK_CHANGE:
		PMD_DRV_LOG(DEBUG, "VIRTCHNL_EVENT_LINK_CHANGE event");
		iavf_handle_link_change_event(dev, pf_msg);
		break;
	case VIRTCHNL_EVENT_PF_DRIVER_CLOSE:
		PMD_DRV_LOG(DEBUG, "VIRTCHNL_EVENT_PF_DRIVER_CLOSE event");
		break;
	default:
		PMD_DRV_LOG(ERR, " unknown event received %u", pf_msg->event);
		break;
	}
}

void
iavf_handle_virtchnl_msg(struct rte_eth_dev *dev)
{
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(dev->data.data);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(dev->data.data);
	struct iavf_arq_event_info info;
	uint16_t pending, aq_opc;
	enum virtchnl_ops msg_opc;
	enum iavf_status msg_ret;
	int ret;

	info.buf_len = IAVF_AQ_BUF_SZ;
	info.msg_buf = vf->aq_resp;

	pending = 1;
	while (pending) {
		ret = iavf_clean_arq_element(hw, &info, &pending);

		if (ret != IAVF_SUCCESS) {
			/*
			 * IAVF_ERR_ADMIN_QUEUE_NO_WORK (-57) means AQ is empty
			 * and is a normal way to terminate the drain loop.
			 * Log error only for genuine other failure codes.
			 * Incorrect logging like this during VF resets might
			 * mislead into chasing a non-existent AQ failure.
			 */
			if (ret != IAVF_ERR_ADMIN_QUEUE_NO_WORK)
				PMD_DRV_LOG(INFO, "Failed to read msg from AdminQ,"
					    "ret: %d", ret);
			break;
		}
		aq_opc = rte_le_to_cpu_16(info.desc.opcode);

		/* opcode 0 means the descriptor is empty/zeroed, skip it */
		if (aq_opc == 0)
			continue;

		/* For the message sent from pf to vf, opcode is stored in
		 * cookie_high of struct iavf_aq_desc, while return error code
		 * are stored in cookie_low, Which is done by PF driver.
		 */
		msg_opc = (enum virtchnl_ops)rte_le_to_cpu_32(
						  info.desc.cookie_high);
		msg_ret = (enum iavf_status)rte_le_to_cpu_32(
						  info.desc.cookie_low);
		switch (aq_opc) {
		case iavf_aqc_opc_send_msg_to_vf:
			if (msg_opc == VIRTCHNL_OP_EVENT) {
				iavf_handle_pf_event_msg(dev, info.msg_buf,
						info.msg_len);
			} else {
				/* read message and it's expected one */
				if (msg_opc == vf->pend_cmd) {
					uint32_t cmd_count =
					rte_atomic_fetch_sub_explicit(&vf->pend_cmd_count,
							1, rte_memory_order_relaxed) - 1;
					if (cmd_count == 0)
						iavf_notify_pending_cmd(vf, msg_ret);
				} else {
					PMD_DRV_LOG(ERR,
					"command mismatch, expect %u, get %u",
						vf->pend_cmd, msg_opc);
				}
				PMD_DRV_LOG(DEBUG,
				"adminq response is received, opcode = %d",
						msg_opc);
			}
			break;
		default:
			PMD_DRV_LOG(DEBUG, "Request %u is not supported yet",
				    aq_opc);
			break;
		}
	}
}

#define VIRTCHNL_VERSION_MAJOR_START 1
#define VIRTCHNL_VERSION_MINOR_START 1

/* Check API version with sync wait until version read from admin queue */
int
iavf_check_api_version(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct virtchnl_version_info version, *pver;
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	version.major = VIRTCHNL_VERSION_MAJOR;
	version.minor = VIRTCHNL_VERSION_MINOR;

	args.ops = VIRTCHNL_OP_VERSION;
	args.in_args = (uint8_t *)&version;
	args.in_args_size = sizeof(version);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_INIT_LOG(ERR, "Fail to execute command of OP_VERSION");
		return err;
	}

	pver = (struct virtchnl_version_info *)args.out_buffer;
	vf->virtchnl_version = *pver;

	if (vf->virtchnl_version.major < VIRTCHNL_VERSION_MAJOR_START ||
	    (vf->virtchnl_version.major == VIRTCHNL_VERSION_MAJOR_START &&
	     vf->virtchnl_version.minor < VIRTCHNL_VERSION_MINOR_START)) {
		PMD_INIT_LOG(ERR, "VIRTCHNL API version should not be lower"
			     " than (%u.%u) to support Adaptive VF",
			     VIRTCHNL_VERSION_MAJOR_START,
			     VIRTCHNL_VERSION_MAJOR_START);
		return -1;
	} else if (vf->virtchnl_version.major > VIRTCHNL_VERSION_MAJOR ||
		   (vf->virtchnl_version.major == VIRTCHNL_VERSION_MAJOR &&
		    vf->virtchnl_version.minor > VIRTCHNL_VERSION_MINOR)) {
		PMD_INIT_LOG(ERR, "PF/VF API version mismatch:(%u.%u)-(%u.%u)",
			     vf->virtchnl_version.major,
			     vf->virtchnl_version.minor,
			     VIRTCHNL_VERSION_MAJOR,
			     VIRTCHNL_VERSION_MINOR);
		return -1;
	}

	PMD_DRV_LOG(DEBUG, "Peer is supported PF host");
	return 0;
}

int
iavf_get_vf_resource(struct iavf_adapter *adapter)
{
	struct iavf_hw *hw = IAVF_DEV_PRIVATE_TO_HW(adapter);
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	uint32_t caps, len;
	int err, i;

	args.ops = VIRTCHNL_OP_GET_VF_RESOURCES;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	/* miniosv: no flow subscription, USO, VLAN v2, QoS, inline IPsec or PTP. */
	caps = IAVF_BASIC_OFFLOAD_CAPS | VIRTCHNL_VF_CAP_ADV_LINK_SPEED |
		VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC |
		VIRTCHNL_VF_OFFLOAD_FDIR_PF |
		VIRTCHNL_VF_OFFLOAD_ADV_RSS_PF |
		VIRTCHNL_VF_OFFLOAD_REQ_QUEUES |
		VIRTCHNL_VF_OFFLOAD_CRC |
		VIRTCHNL_VF_LARGE_NUM_QPAIRS;

	args.in_args = (uint8_t *)&caps;
	args.in_args_size = sizeof(caps);

	err = iavf_execute_vf_cmd_safe(adapter, &args);

	if (err) {
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_GET_VF_RESOURCE");
		return -1;
	}

	len =  sizeof(struct virtchnl_vf_resource) +
		      IAVF_MAX_VF_VSI * sizeof(struct virtchnl_vsi_resource);

	memcpy(vf->vf_res, args.out_buffer,
		   RTE_MIN(args.out_size, len));
	/* parse  VF config message back from PF*/
	iavf_vf_parse_hw_config(hw, vf->vf_res);
	for (i = 0; i < vf->vf_res->num_vsis; i++) {
		if (vf->vf_res->vsi_res[i].vsi_type == VIRTCHNL_VSI_SRIOV)
			vf->vsi_res = &vf->vf_res->vsi_res[i];
	}

	if (!vf->vsi_res) {
		PMD_INIT_LOG(ERR, "no LAN VSI found");
		return -1;
	}

	vf->vsi.vsi_id = vf->vsi_res->vsi_id;
	vf->vsi.nb_qps = vf->vsi_res->num_queue_pairs;
	vf->vsi.adapter = adapter;

	return 0;
}

int
iavf_get_supported_rxdid(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int ret;

	args.ops = VIRTCHNL_OP_GET_SUPPORTED_RXDIDS;
	args.in_args = NULL;
	args.in_args_size = 0;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	ret = iavf_execute_vf_cmd_safe(adapter, &args);
	if (ret) {
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_GET_SUPPORTED_RXDIDS");
		return ret;
	}

	vf->supported_rxdid =
		((struct virtchnl_supported_rxdids *)args.out_buffer)->supported_rxdids;

	return 0;
}

int
iavf_enable_queues(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct virtchnl_queue_select queue_select;
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	memset(&queue_select, 0, sizeof(queue_select));
	queue_select.vsi_id = vf->vsi_res->vsi_id;

	queue_select.rx_queues = BIT(adapter->dev_data->nb_rx_queues) - 1;
	queue_select.tx_queues = BIT(adapter->dev_data->nb_tx_queues) - 1;

	args.ops = VIRTCHNL_OP_ENABLE_QUEUES;
	args.in_args = (u8 *)&queue_select;
	args.in_args_size = sizeof(queue_select);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;
	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_ENABLE_QUEUES");
		return err;
	}
	return 0;
}

int
iavf_disable_queues(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct virtchnl_queue_select queue_select;
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	memset(&queue_select, 0, sizeof(queue_select));
	queue_select.vsi_id = vf->vsi_res->vsi_id;

	queue_select.rx_queues = BIT(adapter->dev_data->nb_rx_queues) - 1;
	queue_select.tx_queues = BIT(adapter->dev_data->nb_tx_queues) - 1;

	args.ops = VIRTCHNL_OP_DISABLE_QUEUES;
	args.in_args = (u8 *)&queue_select;
	args.in_args_size = sizeof(queue_select);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;
	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_DISABLE_QUEUES");
		return err;
	}
	return 0;
}

int
iavf_switch_queue(struct iavf_adapter *adapter, uint16_t qid,
		 bool rx, bool on)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct virtchnl_queue_select queue_select;
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	if (adapter->closed)
		return -EIO;

	memset(&queue_select, 0, sizeof(queue_select));
	queue_select.vsi_id = vf->vsi_res->vsi_id;
	if (rx)
		queue_select.rx_queues |= 1 << qid;
	else
		queue_select.tx_queues |= 1 << qid;

	if (on)
		args.ops = VIRTCHNL_OP_ENABLE_QUEUES;
	else
		args.ops = VIRTCHNL_OP_DISABLE_QUEUES;
	args.in_args = (u8 *)&queue_select;
	args.in_args_size = sizeof(queue_select);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;
	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR, "Failed to execute command of %s",
			    on ? "OP_ENABLE_QUEUES" : "OP_DISABLE_QUEUES");
	return err;
}

int
iavf_enable_queues_lv(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct {
		struct virtchnl_del_ena_dis_queues msg;
		struct virtchnl_queue_chunk chunks[IAVF_RXTX_QUEUE_CHUNKS_NUM - 1];
	} queue_req = {};
	struct virtchnl_del_ena_dis_queues *queue_select = &queue_req.msg;
	struct virtchnl_queue_chunk *queue_chunk = queue_select->chunks.chunks;
	struct iavf_cmd_info args;
	int err;

	queue_select->chunks.num_chunks = IAVF_RXTX_QUEUE_CHUNKS_NUM;
	queue_select->vport_id = vf->vsi_res->vsi_id;

	queue_chunk[VIRTCHNL_QUEUE_TYPE_TX].type = VIRTCHNL_QUEUE_TYPE_TX;
	queue_chunk[VIRTCHNL_QUEUE_TYPE_TX].start_queue_id = 0;
	queue_chunk[VIRTCHNL_QUEUE_TYPE_TX].num_queues =
		adapter->dev_data->nb_tx_queues;

	queue_chunk[VIRTCHNL_QUEUE_TYPE_RX].type = VIRTCHNL_QUEUE_TYPE_RX;
	queue_chunk[VIRTCHNL_QUEUE_TYPE_RX].start_queue_id = 0;
	queue_chunk[VIRTCHNL_QUEUE_TYPE_RX].num_queues =
		adapter->dev_data->nb_rx_queues;

	args.ops = VIRTCHNL_OP_ENABLE_QUEUES_V2;
	args.in_args = (u8 *)queue_select;
	args.in_args_size = sizeof(queue_req);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;
	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_ENABLE_QUEUES_V2");

	return err;
}

int
iavf_disable_queues_lv(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct {
		struct virtchnl_del_ena_dis_queues msg;
		struct virtchnl_queue_chunk chunks[IAVF_RXTX_QUEUE_CHUNKS_NUM - 1];
	} queue_req = {};
	struct virtchnl_del_ena_dis_queues *queue_select = &queue_req.msg;
	struct virtchnl_queue_chunk *queue_chunk = queue_select->chunks.chunks;
	struct iavf_cmd_info args;
	int err;

	queue_select->chunks.num_chunks = IAVF_RXTX_QUEUE_CHUNKS_NUM;
	queue_select->vport_id = vf->vsi_res->vsi_id;

	queue_chunk[VIRTCHNL_QUEUE_TYPE_TX].type = VIRTCHNL_QUEUE_TYPE_TX;
	queue_chunk[VIRTCHNL_QUEUE_TYPE_TX].start_queue_id = 0;
	queue_chunk[VIRTCHNL_QUEUE_TYPE_TX].num_queues =
		adapter->dev_data->nb_tx_queues;

	queue_chunk[VIRTCHNL_QUEUE_TYPE_RX].type = VIRTCHNL_QUEUE_TYPE_RX;
	queue_chunk[VIRTCHNL_QUEUE_TYPE_RX].start_queue_id = 0;
	queue_chunk[VIRTCHNL_QUEUE_TYPE_RX].num_queues =
		adapter->dev_data->nb_rx_queues;

	args.ops = VIRTCHNL_OP_DISABLE_QUEUES_V2;
	args.in_args = (u8 *)queue_select;
	args.in_args_size = sizeof(queue_req);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;
	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_DISABLE_QUEUES_V2");

	return err;
}

int
iavf_switch_queue_lv(struct iavf_adapter *adapter, uint16_t qid,
		 bool rx, bool on)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct {
		struct virtchnl_del_ena_dis_queues msg;
	} queue_req = {};
	struct virtchnl_del_ena_dis_queues *queue_select = &queue_req.msg;
	struct virtchnl_queue_chunk *queue_chunk = queue_select->chunks.chunks;
	struct iavf_cmd_info args;
	int err;

	queue_select->chunks.num_chunks = 1;
	queue_select->vport_id = vf->vsi_res->vsi_id;

	if (rx) {
		queue_chunk->type = VIRTCHNL_QUEUE_TYPE_RX;
		queue_chunk->start_queue_id = qid;
		queue_chunk->num_queues = 1;
	} else {
		queue_chunk->type = VIRTCHNL_QUEUE_TYPE_TX;
		queue_chunk->start_queue_id = qid;
		queue_chunk->num_queues = 1;
	}

	if (on)
		args.ops = VIRTCHNL_OP_ENABLE_QUEUES_V2;
	else
		args.ops = VIRTCHNL_OP_DISABLE_QUEUES_V2;
	args.in_args = (u8 *)queue_select;
	args.in_args_size = sizeof(queue_req);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;
	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR, "Failed to execute command of %s",
			    on ? "OP_ENABLE_QUEUES_V2" : "OP_DISABLE_QUEUES_V2");

	return err;
}

int
iavf_configure_rss_lut(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct virtchnl_rss_lut *rss_lut;
	struct iavf_cmd_info args;
	int len, err = 0;

	len = sizeof(*rss_lut) + vf->vf_res->rss_lut_size - 1;
	rss_lut = (struct virtchnl_rss_lut *)calloc(1, len);
	if (!rss_lut)
		return -ENOMEM;

	rss_lut->vsi_id = vf->vsi_res->vsi_id;
	rss_lut->lut_entries = vf->vf_res->rss_lut_size;
	memcpy(rss_lut->lut, vf->rss_lut, vf->vf_res->rss_lut_size);

	args.ops = VIRTCHNL_OP_CONFIG_RSS_LUT;
	args.in_args = (u8 *)rss_lut;
	args.in_args_size = len;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_CONFIG_RSS_LUT");

	free(rss_lut);
	return err;
}

int
iavf_configure_rss_key(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct virtchnl_rss_key *rss_key;
	struct iavf_cmd_info args;
	int len, err = 0;

	len = sizeof(*rss_key) + vf->vf_res->rss_key_size - 1;
	rss_key = (struct virtchnl_rss_key *)calloc(1, len);
	if (!rss_key)
		return -ENOMEM;

	rss_key->vsi_id = vf->vsi_res->vsi_id;
	rss_key->key_len = vf->vf_res->rss_key_size;
	memcpy(rss_key->key, vf->rss_key, vf->vf_res->rss_key_size);

	args.ops = VIRTCHNL_OP_CONFIG_RSS_KEY;
	args.in_args = (u8 *)rss_key;
	args.in_args_size = len;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_CONFIG_RSS_KEY");

	free(rss_key);
	return err;
}

static void
iavf_configure_queue_pair(struct iavf_adapter *adapter,
		struct virtchnl_queue_pair_info *vc_qp,
		uint16_t q_idx)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct ci_rx_queue **rxq = (struct ci_rx_queue **)adapter->dev_data->rx_queues.data();
	struct ci_tx_queue **txq = (struct ci_tx_queue **)adapter->dev_data->tx_queues.data();

	/* common parts */
	vc_qp->txq.vsi_id = vf->vsi_res->vsi_id;
	vc_qp->txq.queue_id = q_idx;

	vc_qp->rxq.vsi_id = vf->vsi_res->vsi_id;
	vc_qp->rxq.queue_id = q_idx;
	vc_qp->rxq.max_pkt_size = vf->max_pkt_len;

	/* is this txq active? */
	if (q_idx < adapter->dev_data->nb_tx_queues) {
		vc_qp->txq.ring_len = txq[q_idx]->nb_tx_desc;
		vc_qp->txq.dma_ring_addr = txq[q_idx]->tx_ring_dma;
	}

	/* is this rxq active? */
	if (q_idx >= adapter->dev_data->nb_rx_queues)
		return;

	vc_qp->rxq.ring_len = rxq[q_idx]->nb_rx_desc;
	vc_qp->rxq.dma_ring_addr = rxq[q_idx]->rx_ring_phys_addr;
	vc_qp->rxq.databuffer_size = rxq[q_idx]->rx_buf_len;
	vc_qp->rxq.crc_disable = rxq[q_idx]->crc_len != 0 ? 1 : 0;
	if (vf->vf_res->vf_cap_flags & VIRTCHNL_VF_OFFLOAD_RX_FLEX_DESC) {
		if (vf->supported_rxdid & RTE_BIT64(rxq[q_idx]->rxdid)) {
			vc_qp->rxq.rxdid = rxq[q_idx]->rxdid;
			PMD_DRV_LOG(NOTICE, "request RXDID[%d] in Queue[%d]",
					vc_qp->rxq.rxdid, q_idx);
		} else {
			PMD_DRV_LOG(NOTICE, "RXDID[%d] is not supported, "
					"request default RXDID[%d] in Queue[%d]",
					rxq[q_idx]->rxdid, IAVF_RXDID_LEGACY_1, q_idx);
			vc_qp->rxq.rxdid = IAVF_RXDID_LEGACY_1;
		}
	}
}

static int
iavf_configure_queue_chunk(struct iavf_adapter *adapter,
		uint16_t chunk_sz,
		uint16_t chunk_start)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct {
		struct virtchnl_vsi_queue_config_info config;
		struct virtchnl_queue_pair_info qp[IAVF_CFG_Q_NUM_PER_BUF];
	} queue_req = {};
	struct iavf_cmd_info args = {};
	struct virtchnl_vsi_queue_config_info *vc_config = &queue_req.config;
	struct virtchnl_queue_pair_info *vc_qp = vc_config->qpair;
	uint16_t chunk_end = chunk_start + chunk_sz;
	uint16_t i;
	size_t buf_len;
	int err;

	if (chunk_sz > IAVF_CFG_Q_NUM_PER_BUF)
		return -EINVAL;

	vc_config->vsi_id = vf->vsi_res->vsi_id;
	vc_config->num_queue_pairs = chunk_sz;

	for (i = chunk_start; i < chunk_end; i++, vc_qp++)
		iavf_configure_queue_pair(adapter, vc_qp, i);

	/* for some reason PF side checks for buffer being too big, so adjust it down */
	buf_len = sizeof(struct virtchnl_vsi_queue_config_info) +
			sizeof(struct virtchnl_queue_pair_info) * chunk_sz;

	args.ops = VIRTCHNL_OP_CONFIG_VSI_QUEUES;
	args.in_args = (uint8_t *)vc_config;
	args.in_args_size = buf_len;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR, "Failed to execute command VIRTCHNL_OP_CONFIG_VSI_QUEUES");
	return err;
}

int
iavf_configure_queues(struct iavf_adapter *adapter, uint16_t num_queue_pairs)
{
	uint16_t c;
	int err;

	/*
	 * we cannot configure all queues in one go because they won't fit into
	 * adminq buffer, so we're going to chunk them instead
	 */
	for (c = 0; c < num_queue_pairs; c += IAVF_CFG_Q_NUM_PER_BUF) {
		uint16_t chunk_sz = RTE_MIN(num_queue_pairs - c, IAVF_CFG_Q_NUM_PER_BUF);
		err = iavf_configure_queue_chunk(adapter, chunk_sz, c);
		if (err) {
			PMD_DRV_LOG(ERR, "Failed to configure queues chunk [%u, %u)",
					c, c + chunk_sz);
			return err;
		}
	}
	return 0;
}

int
iavf_config_irq_map(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct {
		struct virtchnl_irq_map_info map_info;
		struct virtchnl_vector_map vecmap[IAVF_MAX_NUM_QUEUES_DFLT];
	} map_req = {};
	struct virtchnl_irq_map_info *map_info = &map_req.map_info;
	struct iavf_cmd_info args = {};
	int i, err, max_vmi = -1;
	size_t buf_len;

	if (adapter->dev_data->nb_rx_queues > IAVF_MAX_NUM_QUEUES_DFLT) {
		PMD_DRV_LOG(ERR, "number of queues (%u) exceeds the max supported (%u)",
			    adapter->dev_data->nb_rx_queues, IAVF_MAX_NUM_QUEUES_DFLT);
		return -EINVAL;
	}

	for (i = 0; i < adapter->dev_data->nb_rx_queues; i++) {
		struct virtchnl_vector_map *vecmap;
		/* always 0 for 1 MSIX, never bigger than rxq for multi MSIX */
		uint16_t vmi = vf->qv_map[i].vector_id - vf->msix_base;

		/* can't happen but avoid static analysis warnings */
		if (vmi >= IAVF_MAX_NUM_QUEUES_DFLT) {
			PMD_DRV_LOG(ERR, "vector id (%u) exceeds the max supported (%u)",
					vf->qv_map[i].vector_id,
					vf->msix_base + IAVF_MAX_NUM_QUEUES_DFLT - 1);
			return -EINVAL;
		}

		vecmap = &map_info->vecmap[vmi];
		vecmap->vsi_id = vf->vsi_res->vsi_id;
		vecmap->rxitr_idx = IAVF_ITR_INDEX_DEFAULT;
		vecmap->vector_id = vf->qv_map[i].vector_id;
		vecmap->txq_map = 0;
		vecmap->rxq_map |= 1 << vf->qv_map[i].queue_id;

		/* MSIX vectors round robin so look for max */
		if (vmi > max_vmi) {
			map_info->num_vectors++;
			max_vmi = vmi;
		}
	}

	/* for some reason PF side checks for buffer being too big, so adjust it down */
	buf_len = sizeof(struct virtchnl_irq_map_info) +
		  sizeof(struct virtchnl_vector_map) * map_info->num_vectors;

	args.ops = VIRTCHNL_OP_CONFIG_IRQ_MAP;
	args.in_args = (u8 *)map_info;
	args.in_args_size = buf_len;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;
	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command OP_CONFIG_IRQ_MAP");

	return err;
}

static int
iavf_config_irq_map_lv_chunk(struct iavf_adapter *adapter,
		uint16_t chunk_sz,
		uint16_t chunk_start)
{
	struct {
		struct virtchnl_queue_vector_maps map_info;
		struct virtchnl_queue_vector qv_maps[IAVF_CFG_Q_NUM_PER_BUF];
	} chunk_req = {};
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args = {};
	struct virtchnl_queue_vector_maps *map_info = &chunk_req.map_info;
	struct virtchnl_queue_vector *qv_maps = chunk_req.qv_maps;
	size_t buf_len;
	uint16_t i;

	if (chunk_sz > IAVF_CFG_Q_NUM_PER_BUF)
		return -EINVAL;

	map_info->vport_id = vf->vsi_res->vsi_id;
	map_info->num_qv_maps = chunk_sz;
	for (i = 0; i < chunk_sz; i++) {
		qv_maps = &map_info->qv_maps[i];
		qv_maps->itr_idx = VIRTCHNL_ITR_IDX_0;
		qv_maps->queue_type = VIRTCHNL_QUEUE_TYPE_RX;
		qv_maps->queue_id = vf->qv_map[chunk_start + i].queue_id;
		qv_maps->vector_id = vf->qv_map[chunk_start + i].vector_id;
	}

	/* for some reason PF side checks for buffer being too big, so adjust it down */
	buf_len = sizeof(struct virtchnl_queue_vector_maps) +
		  sizeof(struct virtchnl_queue_vector) * (chunk_sz - 1);

	args.ops = VIRTCHNL_OP_MAP_QUEUE_VECTOR;
	args.in_args = (u8 *)map_info;
	args.in_args_size = buf_len;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	return iavf_execute_vf_cmd_safe(adapter, &args);
}

int
iavf_config_irq_map_lv(struct iavf_adapter *adapter, uint16_t num)
{
	uint16_t c;
	int err;

	for (c = 0; c < num; c += IAVF_CFG_Q_NUM_PER_BUF) {
		uint16_t chunk_sz = RTE_MIN(num - c, IAVF_CFG_Q_NUM_PER_BUF);
		err = iavf_config_irq_map_lv_chunk(adapter, chunk_sz, c);
		if (err) {
			PMD_DRV_LOG(ERR, "Failed to configure irq map chunk [%u, %u)",
					c, c + chunk_sz);
			return err;
		}
	}
	return 0;
}

#define IAVF_ETH_ADDR_PER_REQ \
	((IAVF_AQ_BUF_SZ - sizeof(struct virtchnl_ether_addr_list)) / \
	 sizeof(struct virtchnl_ether_addr))
#define IAVF_ETH_ADDR_CMD_SIZE(nb_addrs) \
	(sizeof(struct virtchnl_ether_addr_list) + sizeof(struct virtchnl_ether_addr) * (nb_addrs))

static int
iavf_send_eth_addr_list(struct iavf_adapter *adapter, const char *caller,
		struct virtchnl_ether_addr_list *list, bool add)
{
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	if (list->num_elements > IAVF_ETH_ADDR_PER_REQ) {
		PMD_DRV_LOG(ERR, "cannot fit this ethernet address list in a message to the PF");
		return -EINVAL;
	}

	memset(&args, 0, sizeof(args));
	args.ops = add ? VIRTCHNL_OP_ADD_ETH_ADDR : VIRTCHNL_OP_DEL_ETH_ADDR;
	args.in_args = (uint8_t *)list;
	args.in_args_size = IAVF_ETH_ADDR_CMD_SIZE(list->num_elements);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;
	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err != 0) {
		PMD_DRV_LOG(ERR, "fail to execute command %s for %s",
			add ? "VIRTCHNL_OP_ADD_ETH_ADDR" : "VIRTCHNL_OP_DEL_ETH_ADDR", caller);
		return err;
	}

	PMD_DRV_LOG(DEBUG, "executed command %s for %s",
		add ? "VIRTCHNL_OP_ADD_ETH_ADDR" : "VIRTCHNL_OP_DEL_ETH_ADDR", caller);

	return 0;
}

void
iavf_add_del_secondary_mac_addr(struct iavf_adapter *adapter, bool add)
{
	uint8_t cmd_buffer[IAVF_ETH_ADDR_CMD_SIZE(IAVF_ETH_ADDR_PER_REQ)] = {};
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct virtchnl_ether_addr_list *list;

	list = (struct virtchnl_ether_addr_list *)cmd_buffer;
	list->vsi_id = vf->vsi_res->vsi_id;
	list->num_elements = 0;

	for (unsigned int i = 1; i < RTE_DIM(vf->mac_addrs); i++) {
		struct rte_ether_addr *addr = &vf->mac_addrs[i];
		struct virtchnl_ether_addr *vc_addr = &list->list[list->num_elements];

		if (!rte_is_zero_ether_addr(addr)) {
			list->num_elements++;

			memcpy(vc_addr->addr, addr->addr.data(), RTE_ETHER_ADDR_LEN);
			vc_addr->type = VIRTCHNL_ETHER_ADDR_EXTRA;
		}

		if (list->num_elements == IAVF_ETH_ADDR_PER_REQ) {
			if (iavf_send_eth_addr_list(adapter, __func__, list, add))
				return;
			list->num_elements = 0;
		}
	}

	if (list->num_elements != 0)
		(void)iavf_send_eth_addr_list(adapter, __func__, list, add);
}

int
iavf_query_stats(struct iavf_adapter *adapter,
		struct virtchnl_eth_stats *pstats)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct virtchnl_queue_select q_stats;
	struct iavf_cmd_info args;
	int err;

	if (adapter->closed)
		return -EIO;

	memset(&q_stats, 0, sizeof(q_stats));
	q_stats.vsi_id = vf->vsi_res->vsi_id;
	args.ops = VIRTCHNL_OP_GET_STATS;
	args.in_args = (uint8_t *)&q_stats;
	args.in_args_size = sizeof(q_stats);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_DRV_LOG(ERR, "fail to execute command OP_GET_STATS");
		return err;
	}
	*pstats = *(struct virtchnl_eth_stats *)args.out_buffer;
	return 0;
}

int
iavf_config_promisc(struct iavf_adapter *adapter,
		   bool enable_unicast,
		   bool enable_multicast)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct virtchnl_promisc_info promisc;
	struct iavf_cmd_info args;
	int err;

	if (adapter->closed)
		return -EIO;

	promisc.flags = 0;
	promisc.vsi_id = vf->vsi_res->vsi_id;

	if (enable_unicast)
		promisc.flags |= FLAG_VF_UNICAST_PROMISC;

	if (enable_multicast)
		promisc.flags |= FLAG_VF_MULTICAST_PROMISC;

	args.ops = VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE;
	args.in_args = (uint8_t *)&promisc;
	args.in_args_size = sizeof(promisc);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);

	if (err) {
		PMD_DRV_LOG(ERR,
			    "fail to execute command CONFIG_PROMISCUOUS_MODE");

		if (err == -ENOTSUP)
			return err;

		return -EAGAIN;
	}

	vf->promisc_unicast_enabled = enable_unicast;
	vf->promisc_multicast_enabled = enable_multicast;
	return 0;
}

int
iavf_add_del_eth_addr(struct iavf_adapter *adapter, struct rte_ether_addr *addr,
		     bool add, uint8_t type)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t cmd_buffer[IAVF_ETH_ADDR_CMD_SIZE(1)] = {};
	struct virtchnl_ether_addr_list *list;

	if (adapter->closed)
		return -EIO;

	list = (struct virtchnl_ether_addr_list *)cmd_buffer;
	list->vsi_id = vf->vsi_res->vsi_id;
	list->num_elements = 1;
	list->list[0].type = type;
	memcpy(list->list[0].addr, addr->addr.data(),
		   RTE_ETHER_ADDR_LEN);

	return iavf_send_eth_addr_list(adapter, __func__, list, add);
}

int
iavf_fdir_add(struct iavf_adapter *adapter,
	struct iavf_fdir_conf *filter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct virtchnl_fdir_add *fdir_ret;
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	filter->add_fltr.vsi_id = vf->vsi_res->vsi_id;
	filter->add_fltr.validate_only = 0;

	args.ops = VIRTCHNL_OP_ADD_FDIR_FILTER;
	args.in_args = (uint8_t *)(&filter->add_fltr);
	args.in_args_size = sizeof(*(&filter->add_fltr));
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_DRV_LOG(ERR, "fail to execute command OP_ADD_FDIR_FILTER");
		return err;
	}

	fdir_ret = (struct virtchnl_fdir_add *)args.out_buffer;
	filter->flow_id = fdir_ret->flow_id;

	if (fdir_ret->status == VIRTCHNL_FDIR_SUCCESS) {
		PMD_DRV_LOG(INFO,
			"Succeed in adding rule request by PF");
	} else if (fdir_ret->status == VIRTCHNL_FDIR_FAILURE_RULE_NORESOURCE) {
		PMD_DRV_LOG(ERR,
			"Failed to add rule request due to no hw resource");
		return -1;
	} else if (fdir_ret->status == VIRTCHNL_FDIR_FAILURE_RULE_EXIST) {
		PMD_DRV_LOG(ERR,
			"Failed to add rule request due to the rule is already existed");
		return -1;
	} else if (fdir_ret->status == VIRTCHNL_FDIR_FAILURE_RULE_CONFLICT) {
		PMD_DRV_LOG(ERR,
			"Failed to add rule request due to the rule is conflict with existing rule");
		return -1;
	} else if (fdir_ret->status == VIRTCHNL_FDIR_FAILURE_RULE_INVALID) {
		PMD_DRV_LOG(ERR,
			"Failed to add rule request due to the hw doesn't support");
		return -1;
	} else if (fdir_ret->status == VIRTCHNL_FDIR_FAILURE_RULE_TIMEOUT) {
		PMD_DRV_LOG(ERR,
			"Failed to add rule request due to time out for programming");
		return -1;
	} else {
		PMD_DRV_LOG(ERR,
			"Failed to add rule request due to other reasons");
		return -1;
	}

	return 0;
};

int
iavf_fdir_del(struct iavf_adapter *adapter,
	struct iavf_fdir_conf *filter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct virtchnl_fdir_del *fdir_ret;
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	filter->del_fltr.vsi_id = vf->vsi_res->vsi_id;
	filter->del_fltr.flow_id = filter->flow_id;

	args.ops = VIRTCHNL_OP_DEL_FDIR_FILTER;
	args.in_args = (uint8_t *)(&filter->del_fltr);
	args.in_args_size = sizeof(filter->del_fltr);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_DRV_LOG(ERR, "fail to execute command OP_DEL_FDIR_FILTER");
		return err;
	}

	fdir_ret = (struct virtchnl_fdir_del *)args.out_buffer;

	if (fdir_ret->status == VIRTCHNL_FDIR_SUCCESS) {
		PMD_DRV_LOG(INFO,
			"Succeed in deleting rule request by PF");
	} else if (fdir_ret->status == VIRTCHNL_FDIR_FAILURE_RULE_NONEXIST) {
		PMD_DRV_LOG(ERR,
			"Failed to delete rule request due to this rule doesn't exist");
		return -1;
	} else if (fdir_ret->status == VIRTCHNL_FDIR_FAILURE_RULE_TIMEOUT) {
		PMD_DRV_LOG(ERR,
			"Failed to delete rule request due to time out for programming");
		return -1;
	} else {
		PMD_DRV_LOG(ERR,
			"Failed to delete rule request due to other reasons");
		return -1;
	}

	return 0;
};

int
iavf_fdir_check(struct iavf_adapter *adapter,
		struct iavf_fdir_conf *filter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	struct virtchnl_fdir_add *fdir_ret;
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	filter->add_fltr.vsi_id = vf->vsi_res->vsi_id;
	filter->add_fltr.validate_only = 1;

	args.ops = VIRTCHNL_OP_ADD_FDIR_FILTER;
	args.in_args = (uint8_t *)(&filter->add_fltr);
	args.in_args_size = sizeof(*(&filter->add_fltr));
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_DRV_LOG(ERR, "fail to check flow director rule");
		return err;
	}

	fdir_ret = (struct virtchnl_fdir_add *)args.out_buffer;

	if (fdir_ret->status == VIRTCHNL_FDIR_SUCCESS) {
		PMD_DRV_LOG(INFO,
			"Succeed in checking rule request by PF");
	} else if (fdir_ret->status == VIRTCHNL_FDIR_FAILURE_RULE_INVALID) {
		PMD_DRV_LOG(ERR,
			"Failed to check rule request due to parameters validation"
			" or HW doesn't support");
		err = -1;
	} else {
		PMD_DRV_LOG(ERR,
			"Failed to check rule request due to other reasons");
		err =  -1;
	}

	return err;
}

int
iavf_add_del_rss_cfg(struct iavf_adapter *adapter,
		     struct virtchnl_rss_cfg *rss_cfg, bool add)
{
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	int err;

	memset(&args, 0, sizeof(args));
	args.ops = add ? VIRTCHNL_OP_ADD_RSS_CFG :
		VIRTCHNL_OP_DEL_RSS_CFG;
	args.in_args = (u8 *)rss_cfg;
	args.in_args_size = sizeof(*rss_cfg);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of %s",
			    add ? "OP_ADD_RSS_CFG" :
			    "OP_DEL_RSS_INPUT_CFG");

	return err;
}

int
iavf_get_hena_caps(struct iavf_adapter *adapter, uint64_t *caps)
{
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct virtchnl_rss_hena *hena;
	struct iavf_cmd_info args;
	int err;

	args.ops = VIRTCHNL_OP_GET_RSS_HENA_CAPS;
	args.in_args = NULL;
	args.in_args_size = 0;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_GET_RSS_HENA_CAPS");
		return err;
	}

	hena = (struct virtchnl_rss_hena *)args.out_buffer;
	*caps = hena->hena;
	return 0;
}

int
iavf_set_hena(struct iavf_adapter *adapter, uint64_t hena)
{
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct virtchnl_rss_hena vrh;
	struct iavf_cmd_info args;
	int err;

	vrh.hena = hena;
	args.ops = VIRTCHNL_OP_SET_RSS_HENA;
	args.in_args = (u8 *)&vrh;
	args.in_args_size = sizeof(vrh);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err)
		PMD_DRV_LOG(ERR,
			    "Failed to execute command of OP_SET_RSS_HENA");

	return err;
}

int
iavf_request_queues(struct rte_eth_dev *dev, uint16_t num)
{
	struct iavf_adapter *adapter =
		IAVF_DEV_PRIVATE_TO_ADAPTER(dev->data.data);
	struct iavf_info *vf =  IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct virtchnl_vf_res_request vfres;
	struct iavf_cmd_info args;
	uint16_t num_queue_pairs;
	int err;
	int i = 0;

	if (!(vf->vf_res->vf_cap_flags &
		VIRTCHNL_VF_OFFLOAD_REQ_QUEUES)) {
		PMD_DRV_LOG(ERR, "request queues not supported");
		return -1;
	}

	if (num == 0) {
		PMD_DRV_LOG(ERR, "queue number cannot be zero");
		return -1;
	}
	vfres.num_queue_pairs = num;

	args.ops = VIRTCHNL_OP_REQUEST_QUEUES;
	args.in_args = (u8 *)&vfres;
	args.in_args_size = sizeof(vfres);
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);

	if (err) {
		PMD_DRV_LOG(ERR, "fail to execute command OP_REQUEST_QUEUES");
		return err;
	}

	/* wait for interrupt notification vf is resetting */
	while (i++ < MAX_TRY_TIMES) {
		if (vf->vf_reset)
			break;
		iavf_msec_delay(ASQ_DELAY_MS);
	}

	/* request queues succeeded, vf is resetting */
	if (vf->vf_reset) {
		PMD_DRV_LOG(INFO, "vf is resetting");
		return 0;
	}

	/* request additional queues failed, return available number */
	num_queue_pairs =
	  ((struct virtchnl_vf_res_request *)args.out_buffer)->num_queue_pairs;
	PMD_DRV_LOG(ERR, "request queues failed, only %u queues "
		"available", num_queue_pairs);

	return -1;
}

int
iavf_get_max_rss_queue_region(struct iavf_adapter *adapter)
{
	struct iavf_info *vf = IAVF_DEV_PRIVATE_TO_VF(adapter);
	uint8_t msg_buf[IAVF_AQ_BUF_SZ] = {};
	struct iavf_cmd_info args;
	uint16_t qregion_width;
	int err;

	args.ops = VIRTCHNL_OP_GET_MAX_RSS_QREGION;
	args.in_args = NULL;
	args.in_args_size = 0;
	args.out_buffer = msg_buf;
	args.out_size = IAVF_AQ_BUF_SZ;

	err = iavf_execute_vf_cmd_safe(adapter, &args);
	if (err) {
		PMD_DRV_LOG(ERR, "Failed to execute command of VIRTCHNL_OP_GET_MAX_RSS_QREGION");
		return err;
	}

	qregion_width =
	((struct virtchnl_max_rss_qregion *)args.out_buffer)->qregion_width;

	vf->max_rss_qregion = (uint16_t)(1 << qregion_width);

	return 0;
}


