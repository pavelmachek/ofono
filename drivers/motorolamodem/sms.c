/* -*- linux-c -*-
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2019  Pavel Machek <pavel@ucw.cz>.
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

#include <glib.h>
#include <ell/ell.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sms.h>
#include "smsutil.h"
#include "util.h"

#include "gatchat.h"
#include "motchat.h"
#include "gatresult.h"

#include "motorolamodem.h"

static const char *none_prefix[] = { NULL };

struct sms_data {
	GMotChat *chat, *send_chat;

	struct cb_data *cbd; /* callback data for... FIXME: unused? */
};

static void motorola_cmgs_cb(GAtResult *result, gpointer user_data)
{
  	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);
	struct cb_data *cbd = data->cbd;
	GAtResultIter iter;
	ofono_sms_submit_cb_t cb = cbd->cb;
	struct ofono_error error;
	int mr;

	DBG("");
	data->cbd = NULL;

	DBG("iter init");
	g_at_result_iter_init(&iter, result);

	/* FIXME: this needs to be U0000...?! */
	if (!g_at_result_iter_next(&iter, "+GCMGS="))
		goto err;

	DBG("next");
	if (!g_at_result_iter_next_number(&iter, &mr))
		goto err;

	DBG("get number");
	DBG("Got MR: %d", mr);

	cb(&error, mr, cbd->data);
	return;

err:
	DBG("callback with failure");
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void motorola_send_pdu(struct ofono_sms *sms, const unsigned char *pdu,
			      int pdu_len)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	char buf[512], buf_pdu[512];

	DBG("");

	/*                          AT+GCMGS */
	snprintf(buf, sizeof(buf), "U0000AT+GCMGS=\r");
	DBG("CMGS intro is %s", buf);
	
	DBG("pdu len %d", pdu_len);
	encode_hex_own_buf(pdu, pdu_len, 0, buf_pdu);
	//strcat(buf, buf_pdu+2);
	strcat(buf_pdu, "\x1a\r");
	buf_pdu[1] = 'U';
	//DBG("Complete command is %s", buf);

#if 0
	g_mot_chat_send(data->send_chat, buf, none_prefix, NULL, data, NULL);
	g_mot_chat_send(data->send_chat, buf_pdu+2, none_prefix, NULL, data, NULL);
#else
	g_at_io_write(data->send_chat->parent->io, buf, strlen(buf));
	g_io_channel_flush(data->send_chat->parent->io->channel, NULL);
	g_at_io_write(data->send_chat->parent->io, buf_pdu+1, strlen(buf_pdu)-1);
	g_io_channel_flush(data->send_chat->parent->io->channel, NULL);
#endif
	return;
}


static void motorola_cmgs(struct ofono_sms *sms, const unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *user_data)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	if (mms) {
	  DBG("mms likely not supported");
	}

	motorola_send_pdu(sms, pdu, pdu_len);
	data->cbd = cbd;
	return;
/*
	if (g_mot_chat_send(data->send_chat, buf, cmgs_prefix,
				motorola_cmgs_cb, cbd, g_free) > 0)
		return;
*/
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, user_data);
}

static void at_cnma_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	DBG("at+cnma: we really need this");
	if (!ok)
		ofono_error("CNMA acknowledgement failed: "
				"Further SMS reception is not guaranteed");
}

static inline void motorola_ack_delivery(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	char buf[256];

	DBG("");
	snprintf(buf, sizeof(buf), "U0000AT+GCNMA=1");
	/* GCNMA=1 on _outsms_ may be doing the trick?
	   Tony does GCNMA on insms. (and suggest CNMA=0,0; sometimes) */

	strcat(buf, "\r");
	g_at_io_write(data->send_chat->parent->io, buf, strlen(buf));
	g_io_channel_flush(data->send_chat->parent->io->channel, NULL);
}

static void got_hex_pdu(struct ofono_sms *sms, const char *hexpdu)
{
	long pdu_len;
	int tpdu_len;
  	unsigned char pdu[176];

	if (hexpdu[0] == '~') {
		printf("PDU will be on next line?\n");
		return;
	}

	if (strlen(hexpdu) > sizeof(pdu) * 2) {
		ofono_error("Bad PDU length in CDS notification");
		return;
	}

	/* Decode pdu and notify about new SMS status report */
	decode_hex_own_buf(hexpdu, -1, &pdu_len, 0, pdu);
	tpdu_len = pdu_len - pdu[0] - 1; /* Matches my guess, and matches mbimodem */
	DBG("Got new Status-Report PDU via CDS: %s, %d", hexpdu, tpdu_len);
	ofono_sms_deliver_notify(sms, pdu, pdu_len, tpdu_len);
}

static void insms_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_sms *sms = user_data;

	GAtResultIter iter;
	const char *line, *pdu;

	g_at_result_iter_init(&iter, result);
	/* g_at_result_iter_next_hexstring ? */
	if (!g_at_result_iter_next(&iter, ""))
		return;

	line = g_at_result_iter_raw_line(&iter);
	DBG("insms notify:\n %s\n", line);

	pdu = strchr(line, '\r');
	if (!pdu) {
		DBG("Do not have pdu?!\n");
		return;
	}
	pdu += 1;
	DBG("insms notify pdu:\n %s\n", pdu);

	got_hex_pdu(sms, pdu);

	DBG("Acknowledging sms delivery\n");
	if (1)
		motorola_ack_delivery(sms);
}

static int motorola_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
  	struct motorola_sms_params *param = user;
	struct sms_data *data;

	printf("while is DBG() not printed?!\n"); fflush(stdout);
	DBG("**************************** this should be called");

	data = g_new0(struct sms_data, 1);
	data->chat = g_mot_chat_clone(param->receive_chat);
	data->send_chat = g_mot_chat_clone(param->send_chat);

	ofono_sms_set_data(sms, data);

	g_mot_chat_register(data->chat, "U0000~+GCMT", insms_notify, FALSE, sms, NULL);
	g_mot_chat_register(data->send_chat, "U0000+GCMGS", motorola_cmgs_cb, FALSE, sms, NULL);

	/* List of error codes seems to be at

	   https://www.developershome.com/sms/resultCodes2.asp#16.2.1.1
	 */
	return 0;
}

static void motorola_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	g_mot_chat_unref(data->chat);
	g_mot_chat_unref(data->send_chat);
	g_free(data);

	ofono_sms_set_data(sms, NULL);
}

static const struct ofono_sms_driver driver = {
	.name		= "motorolamodem",
	.probe		= motorola_sms_probe,
	.remove		= motorola_sms_remove,
	.submit		= motorola_cmgs,
};

void motorola_sms_init(void)
{
	ofono_sms_driver_register(&driver);
}

void motorola_sms_exit(void)
{
	ofono_sms_driver_unregister(&driver);
}
