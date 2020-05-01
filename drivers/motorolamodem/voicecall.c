/* -*- linux-c -*-
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
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"

#include "motorolamodem.h"

 /* When +VTD returns 0, an unspecified manufacturer-specific delay is used */
#define TONE_DURATION 1000

static const char *none_prefix[] = { NULL };

/* According to 27.007 COLP is an intermediate status for ATD */
static const char *atd_prefix[] = { "+COLP:", NULL };

#define FLAG_NEED_CLIP 1
#define FLAG_NEED_CNAP 2
#define FLAG_NEED_CDIP 4

struct voicecall_data {
	GSList *calls;
	unsigned int local_release;
	unsigned int clcc_source;
	GAtChat *chat;
	unsigned int vendor;
	unsigned int tone_duration;
	guint vts_source;
	unsigned int vts_delay;
	unsigned char flags;
};

struct release_id_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	void *data;
	int id;
};

struct change_state_req {
	struct ofono_voicecall *vc;
	ofono_voicecall_cb_t cb;
	void *data;
	int affected_types;
};

static int class_to_call_type(int cls)
{
	switch (cls) {
	case 1:
		return 0;
	case 4:
		return 2;
	case 8:
		return 9;
	default:
		return 1;
	}
}

static struct ofono_call *create_call(struct ofono_voicecall *vc, int type,
					int direction, int status,
					const char *num, int num_type, int clip)
{
	struct voicecall_data *d = ofono_voicecall_get_data(vc);
	struct ofono_call *call;

	/* Generate a call structure for the waiting call */
	call = g_try_new(struct ofono_call, 1);
	if (call == NULL)
		return NULL;

	ofono_call_init(call);

	call->id = ofono_voicecall_get_next_callid(vc);
	call->type = type;
	call->direction = direction;
	call->status = status;

	if (clip != 2) {
		strncpy(call->phone_number.number, num,
			OFONO_MAX_PHONE_NUMBER_LENGTH);
		call->phone_number.type = num_type;
	}

	call->clip_validity = clip;
	call->cnap_validity = CNAP_VALIDITY_NOT_AVAILABLE;

	d->calls = g_slist_insert_sorted(d->calls, call, at_util_call_compare);

	return call;
}

static void generic_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct change_state_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok && req->affected_types) {
		GSList *l;
		struct ofono_call *call;

		for (l = vd->calls; l; l = l->next) {
			call = l->data;

			if (req->affected_types & (1 << call->status))
				vd->local_release |= (1 << call->id);
		}
	}

	/* We have to callback after we schedule a poll if required */
	req->cb(&error, req->data);
}

static void release_id_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct release_id_req *req = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(req->vc);
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (ok)
		vd->local_release = 1 << req->id;

	/* We have to callback after we schedule a poll if required */
	req->cb(&error, req->data);
}

static void atd_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_voicecall *vc = cbd->user;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	ofono_voicecall_cb_t cb = cbd->cb;
	GAtResultIter iter;
	const char *num;
	int type = 128;
	int validity = 2;
	struct ofono_error error;
	struct ofono_call *call;
	GSList *l;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto out;

	/* On a success, make sure to put all active calls on hold */
	for (l = vd->calls; l; l = l->next) {
		call = l->data;

		if (call->status != CALL_STATUS_ACTIVE)
			continue;

		call->status = CALL_STATUS_HELD;
		ofono_voicecall_notify(vc, call);
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+COLP:")) {
		g_at_result_iter_next_string(&iter, &num);
		g_at_result_iter_next_number(&iter, &type);

		if (strlen(num) > 0)
			validity = 0;
		else
			validity = 2;

		DBG("colp_notify: %s %d %d", num, type, validity);
	}

	/* Generate a voice call that was just dialed, we guess the ID */
	call = create_call(vc, 0, 0, CALL_STATUS_DIALING, num, type, validity);
	if (call == NULL) {
		ofono_error("Unable to malloc, call tracking will fail!");
		return;
	}

	/* oFono core will generate a call with the dialed number
	 * inside its dial callback.  Unless we got COLP information
	 * we do not need to communicate that a call is being
	 * dialed
	 */
	if (validity != 2)
		ofono_voicecall_notify(vc, call);


out:
	cb(&error, cbd->data);
}

static void motorola_dial(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			enum ofono_clir_option clir, ofono_voicecall_cb_t cb,
			void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[256];

	cbd->user = vc;

	if (ph->type == 145)
		snprintf(buf, sizeof(buf), "U0000ATD+%s", ph->number);
	else
		snprintf(buf, sizeof(buf), "U0000ATD%s", ph->number);

	switch (clir) {
	case OFONO_CLIR_OPTION_INVOCATION:
		strcat(buf, ",0");
		break;
	case OFONO_CLIR_OPTION_SUPPRESSION:
		strcat(buf, ",1");
		break;
	default:
		break;
	}

	if (g_at_chat_send(vd->chat, buf, atd_prefix,
				atd_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void motorola_template(const char *cmd, struct ofono_voicecall *vc,
			GAtResultFunc result_cb, unsigned int affected_types,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct change_state_req *req = g_try_new0(struct change_state_req, 1);

	if (req == NULL)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->affected_types = affected_types;

	if (g_at_chat_send(vd->chat, cmd, none_prefix,
				result_cb, req, g_free) > 0)
		return;

error:
	g_free(req);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void motorola_answer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	motorola_template("U0000ATA", vc, generic_cb, 0, cb, data);
}

static void motorola_hangup(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	motorola_template("U0000ATH", vc, generic_cb, 0x3f, cb, data);
}

static void motorola_hold_all_active(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	motorola_template("U0000AT+CHLD=2", vc, generic_cb, 0, cb, data);
}

static void motorola_release_all_held(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	unsigned int held_status = 1 << CALL_STATUS_HELD;
	motorola_template("U0000AT+CHLD=0", vc, generic_cb, held_status, cb, data);
}

static void motorola_set_udub(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	unsigned int incoming_or_waiting =
		(1 << CALL_STATUS_INCOMING) | (1 << CALL_STATUS_WAITING);

	motorola_template("U0000AT+CHLD=0", vc, generic_cb, incoming_or_waiting,
			cb, data);
}

static void motorola_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	motorola_template("U0000AT+CHLD=1", vc, generic_cb, 0x1, cb, data);
}

static void motorola_release_specific(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct release_id_req *req = g_try_new0(struct release_id_req, 1);
	char buf[32];

	if (req == NULL)
		goto error;

	req->vc = vc;
	req->cb = cb;
	req->data = data;
	req->id = id;

	snprintf(buf, sizeof(buf), "U0000AT+CHLD=1%d", id);

	if (g_at_chat_send(vd->chat, buf, none_prefix,
				release_id_cb, req, g_free) > 0)
		return;

error:
	g_free(req);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void motorola_private_chat(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "U0000AT+CHLD=2%d", id);
	motorola_template(buf, vc, generic_cb, 0, cb, data);
}

static void motorola_create_multiparty(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	motorola_template("U0000AT+CHLD=3", vc, generic_cb, 0, cb, data);
}

static void motorola_transfer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	/* Held & Active */
	unsigned int transfer = 0x1 | 0x2;

	/* Transfer can puts held & active calls together and disconnects
	 * from both.  However, some networks support transferring of
	 * dialing/ringing calls as well.
	 */
	transfer |= 0x4 | 0x8;

	motorola_template("U0000AT+CHLD=4", vc, generic_cb, transfer, cb, data);
}

static void motorola_deflect(struct ofono_voicecall *vc,
			const struct ofono_phone_number *ph,
			ofono_voicecall_cb_t cb, void *data)
{
	char buf[128];
	unsigned int incoming_or_waiting =
		(1 << CALL_STATUS_INCOMING) | (1 << CALL_STATUS_WAITING);

	snprintf(buf, sizeof(buf), "U0000AT+CTFR=%s,%d", ph->number, ph->type);
	motorola_template(buf, vc, generic_cb, incoming_or_waiting, cb, data);
}

static gboolean vts_timeout_cb(gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct voicecall_data *vd = cbd->user;
	ofono_voicecall_cb_t cb = cbd->cb;

	vd->vts_source = 0;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	g_free(cbd);

	return FALSE;
}

static void vts_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct voicecall_data *vd = cbd->user;
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, cbd->data);

		g_free(cbd);
		return;
	}

	vd->vts_source = g_timeout_add(vd->vts_delay, vts_timeout_cb, cbd);
}

static void motorola_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
			ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);
	int len = strlen(dtmf);
	int s;
	int i;
	char *buf;

	cbd->user = vd;

	/* strlen("+VTS=T;") = 7 + initial AT + null */
	buf = g_try_new(char, len * 9 + 3);
	if (buf == NULL)
		goto error;

	s = sprintf(buf, "U0000AT+VTS=%c", dtmf[0]);

	for (i = 1; i < len; i++)
		s += sprintf(buf + s, ";+VTS=%c", dtmf[i]);

	vd->vts_delay = vd->tone_duration * len;

	s = g_at_chat_send(vd->chat, buf, none_prefix,
				vts_cb, cbd, NULL);

	g_free(buf);

	if (s > 0)
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ring_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct ofono_call *call;

	/* See comment in CRING */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_WAITING),
				at_util_call_compare_by_status))
		return;

	/* RING can repeat, ignore if we already have an incoming call */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status))
		return;

	/* Generate an incoming call of unknown type */
	call = create_call(vc, 9, 1, CALL_STATUS_INCOMING, NULL, 128, 2);
	if (call == NULL) {
		ofono_error("Couldn't create call, call management is fubar!");
		return;
	}

	/* We don't know the call type, we must run clcc */
	vd->flags = FLAG_NEED_CLIP | FLAG_NEED_CNAP | FLAG_NEED_CDIP;
}

static void cring_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *line;
	int type;

	/* Handle the following situation:
	 * Active Call + Waiting Call.  Active Call is Released.  The Waiting
	 * call becomes Incoming and RING/CRING indications are signaled.
	 * Sometimes these arrive before we managed to poll CLCC to find about
	 * the stage change.  If this happens, simply ignore the RING/CRING
	 * when a waiting call exists (cannot have waiting + incoming in GSM)
	 */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_WAITING),
				at_util_call_compare_by_status))
		return;

	/* CRING can repeat, ignore if we already have an incoming call */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status))
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CRING:"))
		return;

	line = g_at_result_iter_raw_line(&iter);
	if (line == NULL)
		return;

	/* Ignore everything that is not voice for now */
	if (!strcasecmp(line, "VOICE"))
		type = 0;
	else
		type = 9;

	/* Generate an incoming call */
	create_call(vc, type, 1, CALL_STATUS_INCOMING, NULL, 128, 2);

	/* We have a call, and call type but don't know the number and
	 * must wait for the CLIP to arrive before announcing the call.
	 * So we wait, and schedule the clcc call.  If the CLIP arrives
	 * earlier, we announce the call there
	 */
	vd->flags = FLAG_NEED_CLIP | FLAG_NEED_CNAP | FLAG_NEED_CDIP;

	DBG("");
}

static void clip_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *num;
	int type, validity;
	GSList *l;
	struct ofono_call *call;

	printf("got clip, searching for incoming calls\n");

	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status);
	if (l == NULL) {
		ofono_error("CLIP for unknown call");
		return;
	}

	/* We have already saw a CLIP for this call, no need to parse again */
	if ((vd->flags & FLAG_NEED_CLIP) == 0)
		return;

	g_at_result_iter_init(&iter, result);

	printf("Got clip...\n");

	if (/* !g_at_result_iter_next(&iter, "+CLIP:") && */
	    !g_at_result_iter_next(&iter, "~+CLIP="))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &type))
		return;

	if (strlen(num) > 0)
		validity = CLIP_VALIDITY_VALID;
	else
		validity = CLIP_VALIDITY_NOT_AVAILABLE;

	/* Skip subaddr, satype and alpha */
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	/* If we have CLI validity field, override our guessed value */
	g_at_result_iter_next_number(&iter, &validity);

	DBG("%s %d %d", num, type, validity);

	call = l->data;

	strncpy(call->phone_number.number, num,
		OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->phone_number.type = type;
	call->clip_validity = validity;

	if (call->type == 0)
		ofono_voicecall_notify(vc, call);

	vd->flags &= ~FLAG_NEED_CLIP;
}

static void cdip_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *num;
	int type;
	GSList *l;
	struct ofono_call *call;

	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status);
	if (l == NULL) {
		ofono_error("CDIP for unknown call");
		return;
	}

	/* We have already saw a CDIP for this call, no need to parse again */
	if ((vd->flags & FLAG_NEED_CDIP) == 0)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CDIP:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &type))
		return;

	DBG("%s %d", num, type);

	call = l->data;

	strncpy(call->called_number.number, num,
		OFONO_MAX_PHONE_NUMBER_LENGTH);
	call->called_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call->called_number.type = type;

	/* Only signal the call here if we already signaled it to the core */
	if (call->type == 0 && (vd->flags & FLAG_NEED_CLIP) == 0)
		ofono_voicecall_notify(vc, call);

	vd->flags &= ~FLAG_NEED_CDIP;
}

static void cnap_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *name;
	int validity;
	GSList *l;
	struct ofono_call *call;

	l = g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_INCOMING),
				at_util_call_compare_by_status);
	if (l == NULL) {
		ofono_error("CNAP for unknown call");
		return;
	}

	/* We have already saw a CLIP for this call, no need to parse again */
	if ((vd->flags & FLAG_NEED_CNAP) == 0)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CNAP:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &name))
		return;

	if (strlen(name) > 0)
		validity = CNAP_VALIDITY_VALID;
	else
		validity = CNAP_VALIDITY_NOT_AVAILABLE;

	/* If we have CNI validity field, override our guessed value */
	g_at_result_iter_next_number(&iter, &validity);

	DBG("%s %d", name, validity);

	call = l->data;

	strncpy(call->name, name,
		OFONO_MAX_CALLER_NAME_LENGTH);
	call->name[OFONO_MAX_CALLER_NAME_LENGTH] = '\0';
	call->cnap_validity = validity;

	/* Only signal the call here if we already signaled it to the core */
	if (call->type == 0 && (vd->flags & FLAG_NEED_CLIP) == 0)
		ofono_voicecall_notify(vc, call);

	vd->flags &= ~FLAG_NEED_CNAP;
}

static void ccwa_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	const char *num;
	int num_type, validity, cls;
	struct ofono_call *call;

	/* Some modems resend CCWA, ignore it the second time around */
	if (g_slist_find_custom(vd->calls,
				GINT_TO_POINTER(CALL_STATUS_WAITING),
				at_util_call_compare_by_status))
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CCWA:"))
		return;

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (!g_at_result_iter_next_number(&iter, &num_type))
		return;

	if (!g_at_result_iter_next_number(&iter, &cls))
		return;

	/* Skip alpha field */
	g_at_result_iter_skip_next(&iter);

	if (strlen(num) > 0)
		validity = 0;
	else
		validity = 2;

	/* If we have CLI validity field, override our guessed value */
	g_at_result_iter_next_number(&iter, &validity);

	DBG("%s %d %d %d", num, num_type, cls, validity);

	call = create_call(vc, class_to_call_type(cls), 1, CALL_STATUS_WAITING,
				num, num_type, validity);
	if (call == NULL) {
		ofono_error("Unable to malloc. Call management is fubar");
		return;
	}

	if (call->type == 0) /* Only notify voice calls */
		ofono_voicecall_notify(vc, call);
}

static void cssi_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	GAtResultIter iter;
	int code, index;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSI:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		index = 0;

	ofono_voicecall_ssn_mo_notify(vc, 0, code, index);
}

static void cssu_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	GAtResultIter iter;
	int code;
	int index;
	const char *num;
	struct ofono_phone_number ph;

	ph.number[0] = '\0';
	ph.type = 129;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSU:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code))
		return;

	if (!g_at_result_iter_next_number_default(&iter, -1, &index))
		goto out;

	if (!g_at_result_iter_next_string(&iter, &num))
		goto out;

	strncpy(ph.number, num, OFONO_MAX_PHONE_NUMBER_LENGTH);

	if (!g_at_result_iter_next_number(&iter, &ph.type))
		return;

out:
	ofono_voicecall_ssn_mt_notify(vc, 0, code, index, &ph);
}


static void ciev_notify(GAtResult *result, gpointer user_data)
{
  	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	int strength, ind;
	GAtResultIter iter;
	struct ofono_call *call;
	enum ofono_disconnect_reason reason;

	g_at_result_iter_init(&iter, result);

	printf("Got ciev...\n");
	if (!g_at_result_iter_next(&iter, "U0000~+CIEV="))
		return;

	if (!g_at_result_iter_next_number(&iter, &ind))
		return;

	if (ind != 1)
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	printf("Got ciev 1,%d...: \n", strength);

	switch (strength) {
	case 7: /* outgoing call starts */
		printf("Outgoing notification, but ATD should have created it for us\n");
		break;
	case 4: /* call incoming ringing */
		printf("Call ringing\n");
		call = create_call(vc, 9, 1, CALL_STATUS_INCOMING, NULL, 128, 2);
		if (call == NULL) {
			ofono_error("Couldn't create call, call management is fubar!");
			return;
		}
		call->type = 0;
		vd->flags = FLAG_NEED_CLIP;
		/* FIXME: we should really do that at +CLIP callback .. when that works */
		//ofono_voicecall_notify(vc, call);
		break;
	case 0: /* call ends */
		call = vd->calls->data;
	  
		reason = OFONO_DISCONNECT_REASON_REMOTE_HANGUP;
		if (!call->type)
			ofono_voicecall_disconnected(vc, call->id, reason, NULL);

		printf("Call ends\n"); break;
	}	   
}


static void motorola_voicecall_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	DBG("voicecall_init: registering to notifications");

	g_at_chat_register(vd->chat, "U0000RING", ring_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "U0000+CRING:", cring_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "U0000+CLIP:", clip_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "U0000~+CLIP=", clip_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "U0000~+CIEV=", ciev_notify, FALSE, vc, NULL);
	
	g_at_chat_register(vd->chat, "U0000+CDIP:", cdip_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "U0000+CNAP:", cnap_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "U0000+CCWA:", ccwa_notify, FALSE, vc, NULL);

	g_at_chat_register(vd->chat, "U0000+CSSI:", cssi_notify, FALSE, vc, NULL);
	g_at_chat_register(vd->chat, "U0000+CSSU:", cssu_notify, FALSE, vc, NULL);

	ofono_voicecall_register(vc);
}

static int motorola_voicecall_probe(struct ofono_voicecall *vc, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->chat = g_at_chat_clone(chat);
	vd->vendor = vendor;
	vd->tone_duration = TONE_DURATION;

	ofono_voicecall_set_data(vc, vd);

	g_at_chat_send(vd->chat, "U0000AT+CLIP=1", NULL, NULL, NULL, NULL);

	g_at_chat_send(vd->chat, "U0000AT+CCWA=1", NULL,
				motorola_voicecall_initialized, vc, NULL);

	return 0;
}

static void motorola_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	if (vd->clcc_source)
		g_source_remove(vd->clcc_source);

	if (vd->vts_source)
		g_source_remove(vd->vts_source);

	g_slist_free_full(vd->calls, g_free);

	ofono_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static const struct ofono_voicecall_driver driver = {
	.name			= "motorolamodem",
	.probe			= motorola_voicecall_probe,
	.remove			= motorola_voicecall_remove,
	.dial			= motorola_dial,
	.answer			= motorola_answer,
	.hangup_all		= motorola_hangup,
	.hold_all_active	= motorola_hold_all_active,
	.release_all_held	= motorola_release_all_held,
	.set_udub		= motorola_set_udub,
	.release_all_active	= motorola_release_all_active,
	.release_specific	= motorola_release_specific,
	.private_chat		= motorola_private_chat,
	.create_multiparty	= motorola_create_multiparty,
	.transfer		= motorola_transfer,
	.deflect		= motorola_deflect,
	.swap_without_accept	= NULL,
	.send_tones		= motorola_send_dtmf
};

void motorola_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void motorola_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
