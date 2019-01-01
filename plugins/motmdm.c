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

#include <stdlib.h>
#include <errno.h>
#include <termios.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>
#include <gatmux.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/call-volume.h>
#include <ofono/devinfo.h>
#include <ofono/message-waiting.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>
#include <ofono/stk.h>

#include "gatio.h"
#include "gatchat.h"

#include <drivers/atmodem/vendor.h>

#define NUM_DLC 3

#define VOICE_DLC   0
#define NETREG_DLC  1
#define SMS_DLC     2

static char *debug_prefixes[NUM_DLC] = { "Voice: ", "Net: ", "SMS: " };

struct motmdm_data {
	GAtChat *dlcs[NUM_DLC];
	gboolean phonebook_added;
	gboolean sms_added;
	gboolean have_sim;
	struct ofono_sim *sim;
};

#define NUM_DLC 1 /* HACK */

const int use_usb = 0;

static const char *cpin_prefix[] = { "+CPIN:", NULL };
static const char *cfun_prefix[] = { "+CFUN:", NULL };
static const char *scrn_prefix[] = { "+SCRN:", NULL };
static const char *none_prefix[] = { NULL };

static void motmdm_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	DBG("motmdm_debug -- ####################################################\n");
	ofono_info("%s%s", prefix, str);
}

static int motmdm_probe(struct ofono_modem *modem)
{
	const char *device;
	struct motmdm_data *data;

	DBG("motmdm: probe\n");

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL)
		return -EINVAL;

	DBG("%s", device);

	data = g_new0(struct motmdm_data, 1);

	ofono_modem_set_data(modem, data);

	return 0;
}

static void motmdm_remove(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_free(data);
}

static void cstat_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct calypso_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	const char *stat;
	int enabled;

	//DBG("signal changes\n");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "~+RSSI="))
		return;

	for (int i = 0; i < 7; i++) {
	/* 7 numbers */
	  if (!g_at_result_iter_next_number(&iter, &enabled))
	    return;
	  //DBG("signal changes %d %d\n", i, enabled);
	}
}


static void cfun_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("");
}

static void scrn_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("");
}

static void setup_modem(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("setup_modem !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! start\n");

	/* AT+SCRN=0 to disable notifications. */
	/* Test parsing of incoming stuff */

	/* CSTAT tells us when SMS & Phonebook are ready to be used */
	g_at_chat_register(data->dlcs[VOICE_DLC], "~+RSSI=", cstat_notify,
				FALSE, modem, NULL);
	g_at_chat_send(data->dlcs[VOICE_DLC], "AT+SCRN=0", scrn_prefix, scrn_cb, modem, NULL);	
	g_at_chat_send(data->dlcs[VOICE_DLC], "ATE0", NULL, NULL, modem, NULL);	
	g_at_chat_send(data->dlcs[VOICE_DLC], "AT+CFUN=1", cfun_prefix, cfun_cb, modem, NULL);



	//write(fd, "AT+SCRN=0\r\n", 11);
	//g_at_io_write(data->dlcs[VOICE_DLC]->io, "AT+SCRN=0\r\n", 11);

	DBG("setup_modem !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! done\n");
}

static void simpin_check_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("");

	/* Modem returns ERROR if there is no SIM in slot. */
	data->have_sim = ok;

	setup_modem(modem);

	ofono_modem_set_powered(modem, TRUE);
}

static void init_simpin_check(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);

	/*
	 * Check for SIM presence by seeing if AT+CPIN? succeeds.
	 * The SIM can not be practically inserted/removed without
	 * restarting the device so there's no need to check more
	 * than once.
	 */
	g_at_chat_send(data->dlcs[VOICE_DLC], "AT+CPIN?", cpin_prefix,
			simpin_check_cb, modem, NULL);
}

static void modem_initialize(struct ofono_modem *modem)
{
	GAtSyntax *syntax;
	GAtChat *chat;
	const char *device;
	GIOChannel *io;
	GHashTable *options;
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("");

	device = ofono_modem_get_string(modem, "Device");

	options = g_hash_table_new(g_str_hash, g_str_equal);
	if (options == NULL)
		goto error;

	g_hash_table_insert(options, "Baud", "115200");
	g_hash_table_insert(options, "Parity", "none");
	g_hash_table_insert(options, "StopBits", "1");
	g_hash_table_insert(options, "DataBits", "8");
	g_hash_table_insert(options, "XonXoff", "off");
	g_hash_table_insert(options, "Local", "off");
	g_hash_table_insert(options, "RtsCts", "off");

	int fd = 999;
	if (!use_usb) {
		device = "/dev/motmdm1"; /* Not a tty device */
		fd = open(device, O_RDWR);
		io = g_io_channel_unix_new(fd);
 	} else {
		device = "/dev/ttyUSB4";
		io = g_at_tty_open(device, options);
	}
	DBG("opening %s\n", device);

	g_hash_table_destroy(options);

	if (io == NULL)
		goto error;

	/* 
	 */
	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(io);

	if (chat == NULL)
		goto error;

	DBG("modem initialized?\n");

	//g_at_chat_set_wakeup_command(chat, "AT\n\r", 500, 5000);

	for (int i = 0; i < NUM_DLC; i++) {
		data->dlcs[i] = chat;

		if (getenv("OFONO_AT_DEBUG")) {
			DBG("debugging enabled\n");
			g_at_chat_set_debug(data->dlcs[i], motmdm_debug,
					    debug_prefixes[i]);
		}
		else
			DBG("debug not enabled\n");
	}

	setup_modem(modem);

	ofono_modem_set_powered(modem, TRUE);

	//g_at_chat_send(chat, "AT", NULL, NULL, NULL, NULL);
	//DBG("AT sent?\n");

	return;

error:
	DBG("error in modem_initalize\n");
	ofono_modem_set_powered(modem, FALSE);
}

/* power up hardware */
static int motmdm_enable(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);

	modem_initialize(modem);

	return 0;
}

static int motmdm_disable(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("%p", modem);

	for (i = 0; i < NUM_DLC;  i++) {
		g_at_chat_unref(data->dlcs[i]);
		data->dlcs[i] = NULL;
	}

	data->phonebook_added = FALSE;
	data->sms_added = FALSE;

	return -EINVAL;
}

static void motmdm_pre_sim(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	data->sim = ofono_sim_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_voicecall_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);

	//DBG("Sending CFUN=1\n");
	//g_at_chat_send(data->dlcs[VOICE_DLC], "AT+CFUN=1",
	//		NULL, NULL, NULL, NULL);

	ofono_sim_inserted_notify(data->sim, TRUE);
}

static void motmdm_post_sim(struct ofono_modem *modem)
{
	struct motmdm_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;

	DBG("%p", modem);

	ofono_ussd_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_call_settings_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_netreg_create(modem, OFONO_VENDOR_CALYPSO, "atmodem",
				data->dlcs[NETREG_DLC]);
	ofono_call_meter_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_call_barring_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);
	ofono_call_volume_create(modem, 0, "atmodem", data->dlcs[VOICE_DLC]);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver motmdm_driver = {
	.name		= "motmdm",
	.probe		= motmdm_probe,
	.remove		= motmdm_remove,
	.enable		= motmdm_enable,
	.disable	= motmdm_disable,
	.pre_sim	= motmdm_pre_sim,
	.post_sim	= motmdm_post_sim,
};

static int motmdm_init(void)
{
	ofono_info("motmdm init\n");
	DBG("motmdm init\n");
	return ofono_modem_driver_register(&motmdm_driver);
}

static void motmdm_exit(void)
{
	ofono_modem_driver_unregister(&motmdm_driver);
}

OFONO_PLUGIN_DEFINE(motmdm, "Motorola modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			motmdm_init, motmdm_exit)
