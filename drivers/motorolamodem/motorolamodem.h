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

#include <stdint.h>

struct ofono_modem;

extern int mot_qmi_trigger_events(struct ofono_modem *modem);

extern void motorola_netreg_init(void);
extern void motorola_netreg_exit(void);
extern void motorola_netmon_init(void);
extern void motorola_netmon_exit(void);
extern void motorola_voicecall_init(void);
extern void motorola_voicecall_exit(void);

extern guint mot_at_chat_send(GAtChat *chat, const char *cmd,
				const char **valid_resp, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify);

extern void motorola_sms_init(void);
extern void motorola_sms_exit(void);

struct ofono_sms;

struct motorola_sms_params {
	struct ofono_modem *modem;
	GAtChat *recv;
	GAtChat *xmit;
};

struct motorola_netreg_params {
	struct ofono_netreg *qmi_netreg;
	GAtChat *recv;
};

struct motorola_netmon_params {
	struct ofono_modem *modem;
	GAtChat *recv;
};
