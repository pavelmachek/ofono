/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
 *  Copyright (C) 2017 by sysmocom s.f.m.c. GmbH <info@sysmocom.de>
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
#include <stdbool.h>
#include <string.h>

#include <glib.h>
#include <smsutil.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netmon.h>

#include "gatchat.h"
#include "gatresult.h"
#include "util.h"

#include "motchat.h"
#include "motorolamodem.h"

struct netmon_data {
	struct ofono_modem *modem;
	GMotChat *recv;
};

static void receive_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;
	struct netmon_data *data = ofono_netmon_get_data(netmon);
	GAtResultIter iter;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "~+CREG="))
		return;

	mot_qmi_trigger_events(data->modem);
}

static int motorola_netmon_probe(struct ofono_netmon *netmon,
				unsigned int vendor, void *user_data)
{
	struct motorola_netmon_params *param = user_data;
	struct netmon_data *data;

	DBG("");

	data = g_new0(struct netmon_data, 1);
	if (data == NULL)
		return -ENOMEM;
	data->modem = param->modem;
	data->recv = g_mot_chat_clone(param->recv);
	ofono_netmon_set_data(netmon, data);
	g_mot_chat_register(data->recv, "~+CREG=", receive_notify,
						FALSE, netmon, NULL);

	return 0;
}

static void motorola_netmon_remove(struct ofono_netmon *netmon)
{
	struct netmon_data *data = ofono_netmon_get_data(netmon);

	ofono_netmon_set_data(netmon, NULL);
	g_mot_chat_unref(data->recv);
	g_free(data);
}

static const struct ofono_netmon_driver driver = {
	.name		= "motorolamodem",
	.probe		= motorola_netmon_probe,
	.remove		= motorola_netmon_remove,
};

void motorola_netmon_init(void)
{
	ofono_netmon_driver_register(&driver);
}

void motorola_netmon_exit(void)
{
	ofono_netmon_driver_unregister(&driver);
}
