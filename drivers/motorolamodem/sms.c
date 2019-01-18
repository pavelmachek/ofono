/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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
#include "gatresult.h"

#include "motorolamodem.h"

static const char *csms_prefix[] = { "+CSMS:", NULL };
static const char *none_prefix[] = { NULL };

struct sms_data {
	char *cnma_ack_pdu;
	int cnma_ack_pdu_len;
	GAtChat *chat;
	unsigned int vendor;
};

#if 0
static void motorola_cmgs_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	GAtResultIter iter;
	ofono_sms_submit_cb_t cb = cbd->cb;
	struct ofono_error error;
	int mr;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CMGS:"))
		goto err;

	if (!g_at_result_iter_next_number(&iter, &mr))
		goto err;

	DBG("Got MR: %d", mr);

	cb(&error, mr, cbd->data);
	return;

err:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void motorola_cmgs(struct ofono_sms *sms, const unsigned char *pdu,
			int pdu_len, int tpdu_len, int mms,
			ofono_sms_submit_cb_t cb, void *user_data)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char buf[512];
	int len;

	if (mms) {
		switch (data->vendor) {
		case OFONO_VENDOR_GEMALTO:
			/* no mms support */
			break;
		default:
			snprintf(buf, sizeof(buf), "AT+CMMS=%d", mms);
			g_at_chat_send(data->chat, buf, none_prefix,
					NULL, NULL, NULL);
			break;
		}
	}

	len = snprintf(buf, sizeof(buf), "AT+CMGS=%d\r", tpdu_len);
	encode_hex_own_buf(pdu, pdu_len, 0, buf+len);

	if (g_at_chat_send(data->chat, buf, cmgs_prefix,
				motorola_cmgs_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, user_data);
}
#endif

static void at_cnma_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	if (!ok)
		ofono_error("CNMA acknowledgement failed: "
				"Further SMS reception is not guaranteed");
}

static inline void motorola_ack_delivery(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	char buf[256];

	DBG("");
#if 0
	/* We must acknowledge the PDU using CNMA */
	if (data->cnma_ack_pdu) {
		switch (data->vendor) {
		case OFONO_VENDOR_GEMALTO:
			snprintf(buf, sizeof(buf), "AT+CNMA=1");
			break;
		default:
			snprintf(buf, sizeof(buf), "AT+CNMA=1,%d\r%s",
					data->cnma_ack_pdu_len,
					data->cnma_ack_pdu);
			break;
		}
	} else
#endif
	  {
		/* Should be a safe fallback */
		snprintf(buf, sizeof(buf), "AT+CNMA=0");
	}

	g_at_chat_send(data->chat, buf, none_prefix, at_cnma_cb, NULL, NULL);
}

#if 0
static void motorola_cmt_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);
	GAtResultIter iter;
	const char *hexpdu;
	unsigned char pdu[176];
	long pdu_len;
	int tpdu_len;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CMT:"))
		goto err;

	switch (data->vendor) {
	case OFONO_VENDOR_GEMALTO:
		if (!g_at_result_iter_next_number(&iter, &tpdu_len)) {
			/*
			 * Some Gemalto modems (ALS3,PLS8...), act in
			 * accordance with 3GPP 27.005.  So we need to skip
			 * the first (<alpha>) field
			 *  \r\n+CMT: ,23\r\nCAFECAFECAFE... ...\r\n
			 *             ^------- PDU length
			 */
			DBG("Retrying to find the PDU length");

			if (!g_at_result_iter_skip_next(&iter))
				goto err;

			/* Next attempt at finding the PDU length. */
			if (!g_at_result_iter_next_number(&iter, &tpdu_len))
				goto err;
		}

		break;
	default:
		if (!g_at_result_iter_skip_next(&iter))
			goto err;

		if (!g_at_result_iter_next_number(&iter, &tpdu_len))
			goto err;

		break;
	}

	hexpdu = g_at_result_pdu(result);

	if (strlen(hexpdu) > sizeof(pdu) * 2) {
		ofono_error("Bad PDU length in CMT notification");
		return;
	}

	DBG("Got new SMS Deliver PDU via CMT: %s, %d", hexpdu, tpdu_len);

	decode_hex_own_buf(hexpdu, -1, &pdu_len, 0, pdu);
	ofono_sms_deliver_notify(sms, pdu, pdu_len, tpdu_len);

	if (data->vendor != OFONO_VENDOR_SIMCOM)
		motorola_ack_delivery(sms);
	return;

err:
	ofono_error("Unable to parse CMT notification");
}

#endif

static void motorola_sms_not_supported(struct ofono_sms *sms)
{
	ofono_error("SMS not supported by this modem.  If this is an error"
			" please submit patches to support this hardware");

	ofono_sms_remove(sms);
}

static void construct_ack_pdu(struct sms_data *d)
{
	struct sms ackpdu;
	unsigned char pdu[164];
	int len;
	int tpdu_len;

	DBG("");

	memset(&ackpdu, 0, sizeof(ackpdu));

	ackpdu.type = SMS_TYPE_DELIVER_REPORT_ACK;

	if (!sms_encode(&ackpdu, &len, &tpdu_len, pdu))
		goto err;

	/* Constructing an <ackpdu> according to 27.005 Section 4.6 */
	if (len != tpdu_len)
		goto err;

	d->cnma_ack_pdu = l_util_hexstring(pdu, tpdu_len);
	if (d->cnma_ack_pdu == NULL)
		goto err;

	printf("Have ack PDU: %s\n", d->cnma_ack_pdu);

	d->cnma_ack_pdu_len = tpdu_len;
	return;

err:
	ofono_error("Unable to construct Deliver ACK PDU");
}

static void motorola_csms_query_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);
	gboolean cnma_supported = FALSE;
	GAtResultIter iter;
	int status_min, status_max;
	char buf[128];

	DBG("");

	if (ok) {
	  DBG("we expect failure");
		return motorola_sms_not_supported(sms);
	}

	DBG("Ok, should have error?");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSMS:"))
		goto out;

	if (!g_at_result_iter_open_list(&iter))
		goto out;

	while (g_at_result_iter_next_range(&iter, &status_min, &status_max))
		if (status_min <= 1 && 1 <= status_max)
			cnma_supported = TRUE;

	DBG("CSMS query parsed successfully");

out:
	DBG("");
}

/*  */

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

	/*	for (tpdu_len = 0; tpdu_len < 159; tpdu_len++) */
	  {
	  printf("tpdu_len: %d\n", tpdu_len);
	/* Decode pdu and notify about new SMS status report */
	  decode_hex_own_buf(hexpdu, -1, &pdu_len, 0, pdu);
	  tpdu_len = pdu_len - 8; /* FIXME: this is not correct */
	DBG("Got new Status-Report PDU via CDS: %s, %d", hexpdu, tpdu_len);
	  ofono_sms_deliver_notify(sms, pdu, pdu_len, tpdu_len);
	}
#if 0
	  construct_ack_pdu(ofono_sms_get_data(sms));
#endif
}

static int registered;

static void insms_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_sms *sms = user_data;

	GAtResultIter iter;
	const char *stat;
	int enabled;
	char *line;

	g_at_result_iter_init(&iter, result);
	/* g_at_result_iter_next_hexstring ? */
	if (!g_at_result_iter_next(&iter, ""))
		return;

	line = g_at_result_iter_raw_line(&iter);
	DBG("insms notify: %s\n", line);

	got_hex_pdu(sms, line);

	if (1)
		motorola_ack_delivery(sms);

#if 0
	if (!g_at_result_iter_next(&iter, "~+RSSI="))
		return;

	for (int i = 0; i < 7; i++) {
	/* 7 numbers */
	  if (!g_at_result_iter_next_number(&iter, &enabled))
	    return;
	  //DBG("signal changes %d %d\n", i, enabled);
	}
#endif
}

static int motorola_sms_probe(struct ofono_sms *sms, unsigned int vendor,
				void *user)
{
	GAtChat *chat = user;
	struct sms_data *data;

	DBG("");

	data = g_new0(struct sms_data, 1);
	data->chat = g_at_chat_clone(chat);
	data->vendor = vendor;

	ofono_sms_set_data(sms, data);

	g_at_chat_register(data->chat, "", insms_notify, FALSE, sms, NULL);

#if 0
	/* Tony says this acks sms, I don't see the effect */
	g_at_chat_send(data->chat, "AT+GCNMA=1", csms_prefix,
			motorola_csms_query_cb, sms, NULL);
	/* Weird. Now it says "+CMS=305". I'm pretty sure it did not do that before? */
	g_at_chat_send(data->chat, "AT+CNMA=0,0", csms_prefix,
			motorola_csms_query_cb, sms, NULL);
#endif

#if 0
	/*	~+GCMT=318\r
                          07912470338016000404B933330011911010122402409BC4B7589E0791CB6E16686E2F83D0E539FB0D72A7D7EF761DE42ECFC965765D4D2FB340613A08FD06B9D36BF21BE42EB7EBFA3248EF2ED7F569BA0B640DCFCB207479CE7E83D620B83C8D6687E765771A447E839A6F719AED1AEB41EA32E8169BD968305018046787E969105CFE068DD373F61B749BD5723498EC367381ACE139A8F916A7D9AEB11E 
AT+GCNMA=1
+CMS=305
AT+GCNMA=1
+GCMS=305
07912470338016000004B933330011911061218380409BC4B7589E0791CB6E16686E2F83D0E539FB0D72A7D7EF761DE4E

07912470338016000004B933330011911061218380409BC4B7589E0791CB6E16686E2F83D0E539FB0D72A7D7EF761DE42
ECFC965765D4D2FB340613A08FD06B9D36BF21BE42EB7EBFA3248EF2ED7F569BA0B640DCFCB207479CE7E83D620B83C8D
6687E765771A447E839A6F719AED1AEB41EA322886CBD16C335018046787E969105CFE068DD373F61B749BD5723498EC3
67381ACE139A8F916A7D9AEB11E

AT+GCNMA=1
0000
:ERROR=4
AT+GCNMA=1
+GCNMA=OK

  


	*/
	got_hex_pdu(sms, "07912470338016000404B933330011911010127042409BC4B7589E0791CB6E16686E2F83D0E539FB0D72A7D7EF761DE42ECFC965765D4D2FB340613A08FD06B9D36BF21BE42EB7EBFA3248EF2ED7F569BA0B640DCFCB207479CE7E83D620B83C8D6687E765771A447E839A6F719AED1AEB41EA326846C3E564315018046787E969105CFE068DD373F61B749BD5723498EC367381ACE139A8F916A7D9AEB11E");
#endif	
	return 0;
}

static void motorola_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	l_free(data->cnma_ack_pdu);

	g_at_chat_unref(data->chat);
	g_free(data);

	ofono_sms_set_data(sms, NULL);
}

static const struct ofono_sms_driver driver = {
	.name		= "motorolamodem",
	.probe		= motorola_sms_probe,
	.remove		= motorola_sms_remove,
#if 0
	.sca_query	= motorola_csca_query,
	.sca_set	= motorola_csca_set,
	.submit		= motorola_cmgs,
	.bearer_query	= motorola_cgsms_query,
	.bearer_set	= motorola_cgsms_set,
#endif
};

void motorola_sms_init(void)
{
	ofono_sms_driver_register(&driver);
}

void motorola_sms_exit(void)
{
	ofono_sms_driver_unregister(&driver);
}
