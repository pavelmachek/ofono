/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <gattty.h>
#include <unistd.h>
#include <stdlib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/netmon.h>
#include <ofono/phonebook.h>
#include <ofono/voicecall.h>
#include <ofono/sim.h>
#include <ofono/stk.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/location-reporting.h>
#include <ofono/log.h>
#include <ofono/message-waiting.h>

#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>

#include <drivers/atmodem/vendor.h>
#include <drivers/qmimodem/qmi.h>
#include <drivers/qmimodem/dms.h>
#include <drivers/qmimodem/wda.h>
#include <drivers/qmimodem/wms.h>
#include <drivers/qmimodem/util.h>
#include <drivers/motorolamodem/motchat.h>
#include <drivers/motorolamodem/motorolamodem.h>

enum motmdm_chat {
	DLC_VOICE,
	DLC_SMS_RECV,
	DLC_SMS_XMIT,
	DLC_SIM,
	USB_AT,
	NUM_CHAT,
};

#define NUM_DLC		(DLC_SIM + 1)

static const char *devices[] = {
	"/dev/gsmtty1",
	"/dev/gsmtty9",
	"/dev/gsmtty3",
	"/dev/gsmtty10",
};

struct motmdm_data {
	struct qmi_device *device;
	struct qmi_service *dms, *wms;
	struct motorola_sim_params mot_sim;
	struct motorola_netreg_params mot_netreg;
	struct motorola_netmon_params mot_netmon;
	struct motorola_sms_params mot_sms;
	GMotChat *chat[NUM_CHAT];
	unsigned long features;
	unsigned int discover_attempts;
	uint8_t oper_mode;
};

static void motmdm_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int motmdm_probe(struct ofono_modem *modem)
{
	struct motmdm_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct motmdm_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void motmdm_remove(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	qmi_service_unref(data->wms);
	qmi_service_unref(data->dms);

	qmi_device_unref(data->device);

	g_free(data);
}

static void shutdown_cb(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("");

	data->discover_attempts = 0;

	qmi_device_unref(data->device);
	data->device = NULL;

	ofono_modem_set_powered(modem, FALSE);
}

static void shutdown_device(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	qmi_service_unref(data->wms);
	qmi_service_unref(data->dms);
	data->dms = NULL;

	qmi_device_shutdown(data->device, shutdown_cb, modem, NULL);
}

static void power_reset_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		shutdown_device(modem);
		return;
	}

	ofono_modem_set_powered(modem, TRUE);
}

static void get_oper_mode_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);
	struct qmi_param *param;
	uint8_t mode;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		shutdown_device(modem);
		return;
	}

	if (!qmi_result_get_uint8(result, QMI_DMS_RESULT_OPER_MODE, &mode)) {
		shutdown_device(modem);
		return;
	}

	data->oper_mode = mode;

	switch (data->oper_mode) {
	case QMI_DMS_OPER_MODE_ONLINE:
		param = qmi_param_new_uint8(QMI_DMS_PARAM_OPER_MODE,
					QMI_DMS_OPER_MODE_PERSIST_LOW_POWER);
		if (!param) {
			shutdown_device(modem);
			return;
		}

		if (qmi_service_send(data->dms, QMI_DMS_SET_OPER_MODE, param,
					power_reset_cb, modem, NULL) > 0)
			return;

		shutdown_device(modem);
		break;
	default:
		ofono_modem_set_powered(modem, TRUE);
		break;
	}
}

static void get_caps_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);
	const struct qmi_dms_device_caps *caps;
	uint16_t len;
	uint8_t i;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	caps = qmi_result_get(result, QMI_DMS_RESULT_DEVICE_CAPS, &len);
	if (!caps)
		goto error;

	DBG("service capabilities %d", caps->data_capa);
	DBG("sim supported %d", caps->sim_supported);

	for (i = 0; i < caps->radio_if_count; i++)
		DBG("radio = %d", caps->radio_if[i]);

	if (qmi_service_send(data->dms, QMI_DMS_GET_OPER_MODE, NULL,
				get_oper_mode_cb, modem, NULL) > 0)
		return;

error:
	shutdown_device(modem);
}

static void create_dms_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!service)
		goto error;

	data->dms = qmi_service_ref(service);

	if (qmi_service_send(data->dms, QMI_DMS_GET_CAPS, NULL,
				get_caps_cb, modem, NULL) > 0)
		return;

error:
	shutdown_device(modem);
}

static void create_shared_dms(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	qmi_service_create_shared(data->device, QMI_SERVICE_DMS,
				  create_dms_cb, modem, NULL);
}

static void create_wms_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!service)
		shutdown_device(modem);

	data->wms = qmi_service_ref(service);
}

static void create_shared_wms(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	qmi_service_create_shared(data->device, QMI_SERVICE_WMS,
				  create_wms_cb, modem, NULL);
}

/* We use a dummy SMSC query to trigger pending qmimodem notifications */
int mot_qmi_trigger_events(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);

	if (data->wms == NULL)
		return -ENODEV;

	return qmi_service_send(data->wms, QMI_WMS_GET_SMSC_ADDR, NULL,
						NULL, NULL, g_free);
};

static void discover_cb(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (qmi_device_is_sync_supported(data->device))
		qmi_device_sync(data->device, create_shared_dms, modem);
	else
		create_shared_dms(modem);

	create_shared_wms(modem);
}

static void motmdm_at_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int motmdm_open_device(struct ofono_modem *modem, const char *device,
				enum motmdm_chat index)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	GIOChannel *channel;
	GMotChat *chat = NULL;

	DBG("device=%s", device);

	channel = g_mot_tty_open(device, NULL);
	if (channel == NULL)
		return -EIO;

	chat = g_mot_chat_new(channel);
	g_io_channel_unref(channel);
	if (chat == NULL)
		return -EIO;

	if (getenv("OFONO_AT_DEBUG"))
		g_mot_chat_set_debug(chat, motmdm_at_debug, "");

	data->chat[index] = chat;

	return 0;
}

static int motmdm_open_dlc_devices(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	int i, err, found = 0;
	GMotChat **chat;

	for (i = 0; i < NUM_DLC; i++) {
		chat = &data->chat[i];

		err = motmdm_open_device(modem, devices[i], i);
		if (err < 0) {
			ofono_warn("Could not open dlc%i", i);
			continue;
		}

		switch(i) {
		case DLC_VOICE:
			g_mot_chat_add_delimiter(*chat, ":");
			g_mot_chat_add_hdrlen(*chat, 5);
			g_mot_chat_add_terminator(*chat, "ERROR=", 6, FALSE);
			g_mot_chat_add_terminator(*chat, "+CLCC:", -1, TRUE);
			break;
		case DLC_SMS_XMIT:
		case DLC_SMS_RECV:
			g_mot_chat_add_hdrlen(*chat, 5);
			g_mot_chat_add_terminator(*chat, "+GCMS=305", 10, TRUE);
			g_mot_chat_add_terminator(*chat, "+GCNMA=OK", 9, TRUE);
			g_mot_chat_add_terminator(*chat, "+GCNMA=305", 10, FALSE);
			break;
		case DLC_SIM:
			g_mot_chat_add_hdrlen(*chat, 5);
			g_mot_chat_add_terminator(*chat, "ERROR=", 6, FALSE);
			break;
		default:
			break;
		}

		found++;
	}

	return found;
}

static void motmdm_close_dlc_devices(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	GMotChat *chat;
	int i;

	for (i = 0; i < NUM_DLC; i++) {
		chat = data->chat[i];
		g_mot_chat_cancel_all(chat);
		g_mot_chat_unregister_all(chat);
	}
}

static int motmdm_enable(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	const char *device;
	int fd, err;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device)
		return -EINVAL;

	fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -EIO;

	data->device = qmi_device_new(fd);
	if (!data->device) {
		close(fd);
		return -ENOMEM;
	}

	if (getenv("OFONO_QMI_DEBUG"))
		qmi_device_set_debug(data->device, motmdm_debug, "QMI: ");

	qmi_device_set_close_on_unref(data->device, true);

	qmi_device_discover(data->device, discover_cb, modem, NULL);

	err = motmdm_open_device(modem,
			ofono_modem_get_string(modem, "Modem"),
				USB_AT);
	if (err < 0)
		ofono_warn("Could not open AT modem");

	err = motmdm_open_dlc_devices(modem);
	if (err < NUM_DLC)
		ofono_warn("All DLC freatures not available\n");

	return -EINPROGRESS;
}

static void power_disable_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	shutdown_device(modem);
}

static int motmdm_disable(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	struct qmi_param *param;

	DBG("%p", modem);

	motmdm_close_dlc_devices(modem);

	qmi_service_cancel_all(data->wms);
	qmi_service_unregister_all(data->wms);
	qmi_service_cancel_all(data->dms);
	qmi_service_unregister_all(data->dms);

	param = qmi_param_new_uint8(QMI_DMS_PARAM_OPER_MODE,
				QMI_DMS_OPER_MODE_PERSIST_LOW_POWER);
	if (!param)
		return -ENOMEM;

	if (qmi_service_send(data->dms, QMI_DMS_SET_OPER_MODE, param,
				power_disable_cb, modem, NULL) > 0)
		return -EINPROGRESS;

	shutdown_device(modem);

	return -EINPROGRESS;
}

static void set_online_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	else
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void motmdm_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_param *param;
	uint8_t mode;

	DBG("%p %s", modem, online ? "online" : "offline");

	if (online)
		mode = QMI_DMS_OPER_MODE_ONLINE;
	else
		mode = QMI_DMS_OPER_MODE_LOW_POWER;

	param = qmi_param_new_uint8(QMI_DMS_PARAM_OPER_MODE, mode);
	if (!param)
		goto error;

	if (qmi_service_send(data->dms, QMI_DMS_SET_OPER_MODE, param,
				set_online_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

/* Only some QMI features are usable, voicecall and sms are custom */
static void motmdm_pre_sim(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	struct motorola_sim_params *mot_sim = &data->mot_sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "qmimodem", data->device);
	ofono_sim_create(modem, 0, "qmimodem", data->device);
	mot_sim->modem = modem;
	mot_sim->recv = data->chat[DLC_SIM];
	ofono_sim_create(modem, 0, "motorolamodem", mot_sim);
	ofono_location_reporting_create(modem, 0, "qmimodem", data->device);

	ofono_voicecall_create(modem, OFONO_VENDOR_MOTMDM, "motorolamodem",
					data->chat[DLC_VOICE]);
}

static void motmdm_post_sim(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	struct motorola_sms_params *mot_sms = &data->mot_sms;
	struct ofono_message_waiting *mw;

	DBG("%p", modem);

	ofono_phonebook_create(modem, 0, "qmimodem", data->device);
	ofono_radio_settings_create(modem, 0, "qmimodem", data->device);

	/* Use qmimodem for sending and motorolamodem for receiving */
	mot_sms->modem = modem;
	mot_sms->recv = data->chat[DLC_SMS_RECV];
	mot_sms->xmit = data->chat[DLC_SMS_XMIT];
	ofono_sms_create(modem, 0, "qmimodem", data->device);
	ofono_sms_create(modem, 0, "motorolamodem", mot_sms);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static void motmdm_post_online(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	struct motorola_netreg_params *mot_netreg = &data->mot_netreg;
	struct motorola_netmon_params *mot_netmon = &data->mot_netmon;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	mot_netreg->recv = data->chat[DLC_VOICE];
	mot_netreg->qmi_netreg = ofono_netreg_create(modem, 0, "qmimodem",
							data->device);
	ofono_netreg_create(modem, 0, "motorolamodem", mot_netreg);

	ofono_netmon_create(modem, 0, "qmimodem", data->device);
	mot_netmon->modem = modem;
	mot_netmon->recv = data->chat[DLC_VOICE];
	ofono_netmon_create(modem, 0, "motorolamodem", mot_netmon);

	gprs = ofono_gprs_create(modem, 0, "qmimodem", data->device);
	gc = ofono_gprs_context_create(modem, 0, "qmimodem",
					data->device);
	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);
}

static struct ofono_modem_driver motmdm_driver = {
	.name		= "motmdm",
	.probe		= motmdm_probe,
	.remove		= motmdm_remove,
	.enable		= motmdm_enable,
	.disable	= motmdm_disable,
	.set_online	= motmdm_set_online,
	.pre_sim	= motmdm_pre_sim,
	.post_sim	= motmdm_post_sim,
	.post_online	= motmdm_post_online,
};

static int motmdm_init(void)
{
	return ofono_modem_driver_register(&motmdm_driver);
}

static void motmdm_exit(void)
{
	ofono_modem_driver_unregister(&motmdm_driver);
}

OFONO_PLUGIN_DEFINE(motmdm, "Qualcomm Gobi modem driver", VERSION,
	OFONO_PLUGIN_PRIORITY_DEFAULT, motmdm_init, motmdm_exit)
