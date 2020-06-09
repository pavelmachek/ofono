/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include <drivers/atmodem/atutil.h>

#include "util.h"

#include "gatchat.h"
#include "gatresult.h"

#include "motchat.h"
#include "motorolamodem.h"

struct netreg_data {
	GAtChat *recv;	/* dlc for unsolicited messages */
	struct ofono_netreg *qmi_netreg;
};

static bool motorola_qmi_netreg_available(struct netreg_data *data)
{
	struct ofono_netreg *qmi_netreg = data->qmi_netreg;
	return qmi_netreg && (ofono_netreg_get_data(qmi_netreg) != NULL);
}

/*
 * Signal strength in U1234~+RSSI=0,15,99,99,0,0,0 format, the second
 * number is a percentage.
 */
static void receive_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	struct ofono_netreg *qmi_netreg = data->qmi_netreg;
	GAtResultIter iter;
	int strength;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "~+RSSI="))
		return;

	/* Ignore the first value */
	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	DBG("strength: %i", strength);

	if (motorola_qmi_netreg_available(data))
		ofono_netreg_strength_notify(qmi_netreg, strength);
}

static int motorola_netreg_probe(struct ofono_netreg *netreg,
				unsigned int vendor, void *user_data)
{
	struct motorola_netreg_params *param = user_data;
	struct netreg_data *data;

	data = g_new0(struct netreg_data, 1);
	if (data == NULL)
		return -ENOMEM;
	data->recv = g_mot_chat_clone(param->recv);
	data->qmi_netreg = param->qmi_netreg;
	ofono_netreg_set_data(netreg, data);
	g_at_chat_register(data->recv, "~+RSSI=", receive_notify,
				FALSE, netreg, NULL);

	return 0;
}

static void motorola_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *data = ofono_netreg_get_data(netreg);

	ofono_netreg_set_data(netreg, NULL);
	g_free(data);
}

static const struct ofono_netreg_driver driver = {
	.name			= "motorolamodem",
	.probe			= motorola_netreg_probe,
	.remove			= motorola_netreg_remove,
};

void motorola_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void motorola_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
