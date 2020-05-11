/* -*- linux-c -*-
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
 *  Copyright (c) 2020  Pavel Machek <pavel@ucw.cz>
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

#define DEBUG

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "gatchat.h"
#include "motchat.h"
#include "gatresult.h"

#include "common.h"
#include "motorolamodem.h"

static const char *none_prefix[] = { NULL };
static const char *creg_prefix[] = { "U0000~+CREG=", NULL };

/*
    sudo su
    cat /dev/gsmtty1 &
    printf "U0000AT+CREG?\r" > /dev/gsmtty1
    U0000+CREG=1,11,04CC,0E117D42,0,0,0,0,0,0
*/

struct netreg_data {
	GMotChat *chat;
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int tech;
	struct ofono_network_time time;
	guint nitz_timeout;
	unsigned int vendor;
};

struct tech_query {
	int status;
	int lac;
	int ci;
	struct ofono_netreg *netreg;
};

gboolean mot_util_parse_reg(GAtResult *result, const char *prefix,
				int *mode, int *status,
				int *lac, int *ci, int *tech,
				unsigned int vendor)
{
	GAtResultIter iter;
	int m, s;
	int l = -1, c = -1, t = -1;
	const char *str;

	DBG("1");
	g_at_result_iter_init(&iter, result);

	DBG("2");
	DBG("Data: %s", iter.l->data);

	while (g_at_result_iter_next(&iter, prefix)) {
		gboolean r;

		g_at_result_iter_next_number(&iter, &m);

		DBG("3");		

		int foo;
		g_at_result_iter_next_number(&iter, &foo);

		DBG("have mode?");		
			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == FALSE || strlen(str) != 1)
				continue;

			s = strtol(str, NULL, 10);

			break;

		/* Some firmware will report bogus lac/ci when unregistered */
		if (s != 1 && s != 5)
			goto out;

			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == TRUE)
				l = strtol(str, NULL, 16);
			else
				goto out;

			r = g_at_result_iter_next_unquoted_string(&iter, &str);

			if (r == TRUE)
				c = strtol(str, NULL, 16);
			else
				goto out;

		DBG("parsed ok");

		g_at_result_iter_next_number(&iter, &t);

out:
		DBG("parsed ok or not?");

		
		if (mode)
			*mode = m;

		if (status)
			*status = s;

		if (lac)
			*lac = l;

		if (ci)
			*ci = c;

		if (tech)
			*tech = t;

		return TRUE;
	}

	return FALSE;
}
	
static void at_creg_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb;
	int status, lac, ci, tech;
	struct ofono_error error;
	struct at_netreg_data *nd = cbd->user;

	DBG("creg_cb");
#if 1
	/* FIXME: can't use decode_at_error; it is broken */
	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		DBG("creg_cb: not okay");
		cb(&error, -1, -1, -1, -1, cbd->data);
		return;
	}
	DBG("creg_cb: okay");
#endif

	if (mot_util_parse_reg(result, "U0000+CREG=", NULL, &status,
				&lac, &ci, &tech, 0) == FALSE) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
		return;
	}

	DBG("mot parse : LAC: %x CI: %x: tech: %d", lac, ci, tech);

	if ((status == 1 || status == 5) && (tech == -1)) {
		DBG("FIXME: what tech?");
		
		tech = 0;
	}

	/* 6-10 is EUTRAN, with 8 being emergency bearer case */
	if (status > 5 && tech == -1)
		tech = ACCESS_TECHNOLOGY_EUTRAN;

	cb(&error, status, lac, ci, tech, cbd->data);
}

static void at_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = nd;

	DBG("Sending creg");
	if (g_at_chat_send(nd->chat, "U0000AT+CREG?", creg_prefix,
			   at_creg_cb, cbd, g_free) > 0) {
		DBG("Creg sent ok ok");
		return;
	}

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);
}

static void rssi_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	GAtResultIter iter;
	int strength;

	/* HERE */
	DBG();
	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "U0000~+RSSI="))
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	DBG("have strength!");
	ofono_netreg_strength_notify(netreg,
				at_util_convert_signal_strength(strength));
}

static void creg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int mode;
	int status, lac, ci, tech;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct tech_query *tq;

	/* HERE */
	DBG("");

	if (mot_util_parse_reg(result, "U0000~+CREG=", &mode, &status,
				&lac, &ci, &tech, 0) == FALSE)
		return;

	DBG("");

	if (status != 1 && status != 5)
		goto notify;

	tq = g_try_new0(struct tech_query, 1);
	if (tq == NULL)
		goto notify;

	tq->status = status;
	tq->lac = lac;
	tq->ci = ci;
	tq->netreg = netreg;

	switch (nd->vendor) {
	}

	g_free(tq);

	if ((status == 1 || status == 5) && tech == -1)
		tech = nd->tech;

notify:
	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void at_creg_test_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	gint range[2];
	GAtResultIter iter;
	int creg1 = 0;
	int creg2 = 0;

	DBG("");

	if (!ok) {
		DBG("motmdm -- failure is expected");
		//g_at_chat_register(nd->chat, "+CREG=",
		//		   creg_notify, FALSE, netreg, NULL);
		ofono_netreg_register(netreg);
		return;
	}
	DBG("motmdm -- creg test okay?!");
	ofono_netreg_register(netreg);
	return;
}

static void creg_notify_debug(GAtResult *result, gpointer user_data)
{
	struct netreg_data *nd = user_data;

	GAtResultIter iter;
	const char *line, *pdu;

	g_at_result_iter_init(&iter, result);
	/* g_at_result_iter_next_hexstring ? */
	if (!g_at_result_iter_next(&iter, ""))
		return;

	line = g_at_result_iter_raw_line(&iter);
	DBG("creg notify:\n %s\n", line);
}

static void rssi_notify_debug(GAtResult *result, gpointer user_data)
{
	struct netreg_data *nd = user_data;

	GAtResultIter iter;
	const char *line, *pdu;

	g_at_result_iter_init(&iter, result);
	/* g_at_result_iter_next_hexstring ? */
	if (!g_at_result_iter_next(&iter, ""))
		return;

	line = g_at_result_iter_raw_line(&iter);
	DBG("rssi notify:\n %s\n", line);
}


static int at_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *data)
{
	GMotChat *chat = data;
	struct netreg_data *nd;

	DBG("");

	nd = g_new0(struct netreg_data, 1);

	nd->chat = g_at_chat_clone(chat);
	nd->vendor = vendor;
	nd->tech = -1;
	nd->time.sec = -1;
	nd->time.min = -1;
	nd->time.hour = -1;
	nd->time.mday = -1;
	nd->time.mon = -1;
	nd->time.year = -1;
	nd->time.dst = 0;
	nd->time.utcoff = 0;
	ofono_netreg_set_data(netreg, nd);

	DBG("Probing creg");


	g_mot_chat_register(nd->chat, "U0000~+CREG=", creg_notify, FALSE, nd, NULL);
	g_mot_chat_register(nd->chat, "U0000~+RSSI=", rssi_notify, FALSE, nd, NULL);
	
	g_mot_chat_send(nd->chat, "U0000AT+CREG=?", creg_prefix,
			at_creg_test_cb, netreg, NULL);

	return 0;
}

static void at_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (nd->nitz_timeout)
		g_source_remove(nd->nitz_timeout);

	ofono_netreg_set_data(netreg, NULL);

	g_at_chat_unref(nd->chat);
	g_free(nd);
}

static const struct ofono_netreg_driver driver = {
	.name				= "motorolamodem",
	.probe				= at_netreg_probe,
	.remove				= at_netreg_remove,
//	.registration_status		= at_registration_status,
//	.current_operator		= at_current_operator,
//	.list_operators			= at_list_operators,
//	.register_auto			= at_register_auto,
//	.register_manual		= at_register_manual,
//	.strength			= at_signal_strength,
};

void motorola_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void motorola_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
