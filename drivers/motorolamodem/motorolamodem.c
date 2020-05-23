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

#include <glib.h>
#include <motchat.h>
#include <gatchat.h>
#include <stdio.h>

#include <sys/time.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/types.h>

#include "motorolamodem.h"

guint mot_at_chat_send(GMotChat *chat, const char *cmd,
				const char **valid_resp, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify)
{
	struct timeval now;
	char buf[256];
	uint16_t id;

	gettimeofday(&now, NULL);
	id = (now.tv_sec % 100) * 100;
	id += (now.tv_usec / 1000) / 10;
	snprintf(buf, sizeof(buf), "U%04u%s", id, cmd);

	return g_at_chat_send(chat, buf, valid_resp, func, user_data, notify);
}

static int motorolamodem_init(void)
{
	motorola_netreg_init();
	motorola_netmon_init();
	motorola_voicecall_init();
	motorola_sms_init();
	motorola_sim_init();
	motorola_netreg_init();

	return 0;
}

static void motorolamodem_exit(void)
{
	motorola_netreg_exit();
	motorola_sim_exit();
	motorola_sms_exit();
	motorola_voicecall_exit();
	motorola_netmon_exit();
	motorola_netreg_exit();
}

OFONO_PLUGIN_DEFINE(motorolamodem, "Motorola modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			motorolamodem_init, motorolamodem_exit)
