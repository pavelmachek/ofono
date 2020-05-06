/*
 *
 *  AT chat library with GLib integration
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

#ifndef __GMOTCHAT_H
#define __GMOTCHAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gatresult.h"
#include "gatsyntax.h"
#include "gatutil.h"
#include "gatio.h"

struct _GMotChat;

typedef struct _GMotChat GMotChat;

typedef void (*GAtResultFunc)(gboolean success, GAtResult *result,
				gpointer user_data);
typedef void (*GAtNotifyFunc)(GAtResult *result, gpointer user_data);

enum _GMotChatTerminator {
	G_MOT_CHAT_TERMINATOR_OK,
	G_MOT_CHAT_TERMINATOR_ERROR,
	G_MOT_CHAT_TERMINATOR_NO_DIALTONE,
	G_MOT_CHAT_TERMINATOR_BUSY,
	G_MOT_CHAT_TERMINATOR_NO_CARRIER,
	G_MOT_CHAT_TERMINATOR_CONNECT,
	G_MOT_CHAT_TERMINATOR_NO_ANSWER,
	G_MOT_CHAT_TERMINATOR_CMS_ERROR,
	G_MOT_CHAT_TERMINATOR_CME_ERROR,
	G_MOT_CHAT_TERMINATOR_EXT_ERROR,
};

typedef enum _GMotChatTerminator GMotChatTerminator;

GMotChat *g_mot_chat_new(GIOChannel *channel, GAtSyntax *syntax);
GMotChat *g_mot_chat_new_blocking(GIOChannel *channel, GAtSyntax *syntax);

GIOChannel *g_mot_chat_get_channel(GMotChat *chat);
GAtIO *g_mot_chat_get_io(GMotChat *chat);

GMotChat *g_mot_chat_ref(GMotChat *chat);
void g_mot_chat_unref(GMotChat *chat);

GMotChat *g_mot_chat_clone(GMotChat *chat);

GMotChat *g_mot_chat_set_slave(GMotChat *chat, GMotChat *slave);
GMotChat *g_mot_chat_get_slave(GMotChat *chat);

void g_mot_chat_suspend(GMotChat *chat);
void g_mot_chat_resume(GMotChat *chat);

gboolean g_mot_chat_set_disconnect_function(GMotChat *chat,
			GAtDisconnectFunc disconnect, gpointer user_data);

/*!
 * If the function is not NULL, then on every read/write from the GIOChannel
 * provided to GMotChat the logging function will be called with the
 * input/output string and user data
 */
gboolean g_mot_chat_set_debug(GMotChat *chat,
				GAtDebugFunc func, gpointer user_data);

/*!
 * Queue an AT command for execution.  The command contents are given
 * in cmd.  Once the command executes, the callback function given by
 * func is called with user provided data in user_data.
 *
 * Returns an id of the queued command which can be canceled using
 * g_mot_chat_cancel.  If an error occurred, an id of 0 is returned.
 *
 * This function can be used in three ways:
 * 	- Send a simple command such as g_mot_chat_send(p, "AT+CGMI?", ...
 *
 * 	- Send a compound command: g_mot_chat_send(p, "AT+CMD1;+CMD2", ...
 *
 * 	- Send a command requiring a prompt.  The command up to '\r' is sent
 * 	  after which time a '> ' prompt is expected from the modem.  Further
 * 	  contents of the command are sent until a '\r' or end of string is
 * 	  encountered.  If end of string is encountered, the Ctrl-Z character
 * 	  is sent automatically.  There is no need to include the Ctrl-Z
 * 	  by the caller.
 *
 * The valid_resp field can be used to send an array of strings which will
 * be accepted as a valid response for this command.  This is treated as a
 * simple prefix match.  If a response line comes in from the modem and it
 * does not match any of the prefixes in valid_resp, it is treated as an
 * unsolicited notification.  If valid_resp is NULL, then all response
 * lines after command submission and final response line are treated as
 * part of the command response.  This can be used to get around broken
 * modems which send unsolicited notifications during command processing.
 */
guint g_mot_chat_send(GMotChat *chat, const char *cmd,
				const char **valid_resp, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify);

/*!
 * Same as the above command, except that the caller wishes to receive the
 * intermediate responses immediately through the GAtNotifyFunc callback.
 * The final response will still be sent to GAtResultFunc callback.  The
 * final GAtResult will not contain any lines from the intermediate responses.
 * This is useful for listing commands such as CPBR.
 */
guint g_mot_chat_send_listing(GMotChat *chat, const char *cmd,
				const char **valid_resp,
				GAtNotifyFunc listing, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify);

/*!
 * Same as g_mot_chat_send_listing except every response line in valid_resp
 * is expected to be followed by a PDU.  The listing function will be called
 * with the intermediate response and the following PDU line.
 *
 * This is useful for PDU listing commands like the +CMGL
 */
guint g_mot_chat_send_pdu_listing(GMotChat *chat, const char *cmd,
				const char **valid_resp,
				GAtNotifyFunc listing, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify);

/*!
 * Same as g_mot_chat_send except parser will know to expect short prompt syntax
 * used with +CPOS.
 */
guint g_mot_chat_send_and_expect_short_prompt(GMotChat *chat, const char *cmd,
				const char **valid_resp, GAtResultFunc func,
				gpointer user_data, GDestroyNotify notify);

gboolean g_mot_chat_cancel(GMotChat *chat, guint id);
gboolean g_mot_chat_cancel_all(GMotChat *chat);

gpointer g_mot_chat_get_userdata(GMotChat *chat, guint id);

guint g_mot_chat_register(GMotChat *chat, const char *prefix,
				GAtNotifyFunc func, gboolean expect_pdu,
				gpointer user_data, GDestroyNotify notify);

gboolean g_mot_chat_unregister(GMotChat *chat, guint id);
gboolean g_mot_chat_unregister_all(GMotChat *chat);

gboolean g_mot_chat_set_wakeup_command(GMotChat *chat, const char *cmd,
					guint timeout, guint msec);

void g_mot_chat_add_terminator(GMotChat *chat, char *terminator,
				int len, gboolean success);
void g_mot_chat_blacklist_terminator(GMotChat *chat,
						GMotChatTerminator terminator);

struct _GMotChat {
	gint ref_count;
	struct mot_chat *parent;
	guint group;
	GMotChat *slave;
};

struct mot_chat {
	gint ref_count;				/* Ref count */
	guint next_cmd_id;			/* Next command id */
	guint next_notify_id;			/* Next notify id */
	guint next_gid;				/* Next group id */
	GAtIO *io;				/* AT IO */
	GQueue *command_queue;			/* Command queue */
	guint cmd_bytes_written;		/* bytes written from cmd */
	GHashTable *notify_list;		/* List of notification reg */
	GAtDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	guint read_so_far;			/* Number of bytes processed */
	gboolean suspended;			/* Are we suspended? */
	GAtDebugFunc debugf;			/* debugging output function */
	gpointer debug_data;			/* Data to pass to debug func */
	char *pdu_notify;			/* Unsolicited Resp w/ PDU */
	GSList *response_lines;			/* char * lines of the response */
	char *wakeup;				/* command sent to wakeup modem */
	gint timeout_source;
	gdouble inactivity_time;		/* Period of inactivity */
	guint wakeup_timeout;			/* How long to wait for resp */
	GTimer *wakeup_timer;			/* Keep track of elapsed time */
	GAtSyntax *syntax;
	gboolean destroyed;			/* Re-entrancy guard */
	gboolean in_read_handler;		/* Re-entrancy guard */
	gboolean in_notify;
	GSList *terminator_list;		/* Non-standard terminator */
	guint16 terminator_blacklist;		/* Blacklisted terinators */
};

#ifdef __cplusplus
}
#endif

#endif /* __GATCHAT_H */
