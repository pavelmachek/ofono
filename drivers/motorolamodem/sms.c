/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2019  Pavel Machek <pavel@ucw.cz>
 *  Copyright (C) 2020  Tony Lindgren <tony@atomide.com>
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sms.h>

#include <drivers/atmodem/atutil.h>

#include "smsutil.h"
#include "util.h"

#include "motchat.h"
#include "gatresult.h"

#include "motorolamodem.h"

static const char *gcms_prefix[] = { "+GCMS=", NULL };
static const char *gcnma_prefix[] = { "+GCNMA=", NULL };

struct sms_data {
	struct ofono_modem *modem;
	GMotChat *recv, *xmit;	/* dlc for incoming and outgoing messages */
	unsigned int vendor;
};

static void at_cnma_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	DBG("");

	if (ok)
		return;

	ofono_error("CNMA acknowledgement failed: "
				"Further SMS reception is not guaranteed");
}

/*
 * For acking messages, Android seems to use both AT+CNMA=0,0 and AT+GCNMA=1
 * terminated with '\n' rather than '\r'. Maybe the difference is that
 * AT+GCNMA=1 should be used for GSM and WCDMA while AT+CNMA=0,0 should be
 * used for CDMA networks. Note that the incoming messages are also acked on
 * the recv dlc on Android. However, we can also ack incoming messages on the
 * xmit dlc to avoid mixing PDUs and commands on the recv dlc. Let's also
 * wake the dlc before as otherwise we may not get any response from the
 * modem.
 *
 * Returns "+GCNMA=OK" on success and "+GCMS=305" if nothing to ack.
 */
static void ack_sms_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct sms_data *data = user_data;

	DBG("");

	mot_at_chat_send(data->xmit, "AT+GCNMA=1", gcnma_prefix,
						at_cnma_cb, NULL, NULL);
}

/*
 * Incoming message handling is similar to at_cmgl_notify(). We may need
 * a separate handler for ofono_sms_status_notify() too as we don't seem
 * to have that information with GCMT.
 */
static void receive_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);
	GAtResultIter iter;

	printf("receive_notify:\n");
	DBG("");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "~+GCMT="))
		return;

	printf("qmi trigger:\n");
	if (mot_qmi_trigger_events(data->modem) > 0) {
		DBG("Kicking SMS channel before acking");
		mot_at_chat_send(data->xmit, "AT+GCNMA=?", gcms_prefix,
							ack_sms_cb, data, NULL);
	}
	printf("all done?\n");
}

static void status_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);
	GAtResultIter iter;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "~+GSSR="))
		return;

	mot_qmi_trigger_events(data->modem);
}

static int motorola_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
	struct motorola_sms_params *param = user;
	struct sms_data *data;

	DBG("");
	data = g_new0(struct sms_data, 1);
	data->modem = param->modem;
	data->recv = g_mot_chat_clone(param->recv);
	data->xmit = g_mot_chat_clone(param->xmit);
	data->vendor = vendor;
	ofono_sms_set_data(sms, data);
	g_mot_chat_register(data->recv, "~+GCMT=", receive_notify, FALSE, sms, NULL);
	g_mot_chat_register(data->recv, "~+GSSR=", status_notify, FALSE, sms, NULL);

	return 0;
}

static void motorola_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	DBG("");
	g_mot_chat_unref(data->recv);
	g_mot_chat_unref(data->xmit);
	g_free(data);

	ofono_sms_set_data(sms, NULL);
}

/* See qmimodem for sending messages, submit() is currently not needed */
static const struct ofono_sms_driver driver = {
	.name		= "motorolamodem",
	.probe		= motorola_sms_probe,
	.remove		= motorola_sms_remove,
};

void motorola_sms_init(void)
{
	ofono_sms_driver_register(&driver);
}

void motorola_sms_exit(void)
{
	ofono_sms_driver_unregister(&driver);
}
