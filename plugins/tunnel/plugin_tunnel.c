/* 
 * GSQL - database development tool for GNOME
 *
 * Copyright (C) 2006-2008  Taras Halturin  halturin@gmail.com
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor Boston, MA 02110-1301,  USA
 */


#include <libgsql/plugins.h>
#include <libgsql/stock.h>
#include <gtk/gtk.h>

#include <libssh/libssh.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <libgsql/common.h>
#include "plugin_tunnel.h"
#include "tunnel_conf.h"



#define PLUGIN_VERSION "0.1"
#define PLUGIN_ID    "plugin_tunnel"
#define PLUGIN_NAME  "Tunnel"
#define PLUGIN_DESC  "SSH tunneling"
#define PLUGIN_AUTHOR "Taras Halturin"
#define PLUGIN_HOMEPAGE "http://gsql.org"

/* list of ssh sessions */
static GList *ssh_sessions = NULL;

typedef struct _SSHSession		SSHSession;

#define SSH_SESSION_ERR_LEN	512

#define SSH_SESSION_SET_ERROR(session, params...) \
		memset (session->err, 0, 512); \
		g_snprintf (session->err, 512, params)


struct _SSHSession {

	/* connect to */
	const gchar *hostname;
	const gchar *username;
	const gchar *password;
	guint		port;

	SSH_SESSION *ssh;

	/* listen on */
	const gchar		*localname;
	guint			localport;

	int			sock;

	/* forwaded from */
	const gchar		*fwdhost;
	guint			fwdport;

	/* list of CHANNELs */
	GList		*channels;

	gboolean	connected;
	gchar		err[SSH_SESSION_ERR_LEN];
};

static GSQLStockIcon stock_icons[] = 
{
	{ GSQLP_TUNNEL_STOCK_ICON, "tunnel.png" }
};

static gboolean 
do_open_session (SSHSession *session);

static gboolean
do_open_channel (SSHSession *session);

static gboolean
do_listen_fwd (SSHSession *session);

static void
do_accept_threaded (gpointer data);

gboolean 
plugin_load (GSQLPlugin * plugin)
{
	GSQL_TRACE_FUNC;
	
	plugin->info.author = PLUGIN_AUTHOR;
	plugin->info.id = PLUGIN_ID;
	plugin->info.name = PLUGIN_NAME;
	plugin->info.desc = PLUGIN_DESC;
	plugin->info.homepage = PLUGIN_HOMEPAGE;
	plugin->info.version = PLUGIN_VERSION;
	plugin->file_logo = "tunnel.png";
	
	gsql_factory_add (stock_icons, G_N_ELEMENTS(stock_icons));

	plugin->plugin_conf_dialog = plugin_tunnel_conf_dialog;

	SSHSession *session = g_new0 (SSHSession, 1);

	session->connected = FALSE;
	session->hostname = "192.168.1.33";
	session->port = 22;
	session->username = "fantom";
	session->password = "megapass";
	session->localname = "*";
	session->localport = 10051;
	session->sock = 0;

	do_open_session (session);
	

	return TRUE;
}

gboolean 
plugin_unload (GSQLPlugin * plugin)
{
	GSQL_TRACE_FUNC;

	return TRUE;
}

static gboolean 
do_open_session (SSHSession *session)
{

	SSH_SESSION *ssh = ssh_new();
	SSH_OPTIONS *opts = ssh_options_new();
	

	ssh_options_set_port (opts, session->port);
	ssh_options_set_host (opts, session->hostname);
	
	ssh_set_options (ssh, opts);

	if (ssh_connect(ssh) != SSH_OK)
	{
		//ssh_options_free (opts);
		ssh_disconnect (ssh);

		SSH_SESSION_SET_ERROR (session, "Error at connection :%s\n", ssh_get_error (ssh));
		
		return FALSE;
	}

	ssh_is_server_known(ssh);

	if (ssh_userauth_autopubkey(ssh) != SSH_AUTH_SUCCESS)
	{
		g_debug ("Authenticating with pubkey: %s\n",ssh_get_error(ssh));
		
		if (ssh_userauth_password (ssh, session->username, 
		                           session->password) != SSH_AUTH_SUCCESS)
		{
			SSH_SESSION_SET_ERROR (session, "Authentication with password failed: %s\n",
			                       ssh_get_error (ssh));

			//ssh_options_free (opts);
			ssh_disconnect (ssh);
			
			return FALSE;
		}
	}

	session->ssh = ssh;

	if (!do_listen_fwd (session))
	{
		//ssh_options_free (opts);
		ssh_disconnect (ssh);
		
		return FALSE;
	}

	return TRUE; //session->connected = TRUE;
}

static gboolean
do_open_channel (SSHSession *session)
{
	GSQL_TRACE_FUNC;

	g_return_val_if_fail (session != NULL, FALSE);

	CHANNEL *ch = NULL;
	
	ch = channel_new (session->ssh);

	if (channel_open_forward (ch, session->fwdhost, session->fwdport, 
	                          session->localname, session->localport) != SSH_OK)
	{
		SSH_SESSION_SET_ERROR (session, "Error when opening forward:%s\n", 
		                       ssh_get_error (session->ssh));
		
		return FALSE;
	}

	g_debug ("Chanel is forwarded");

	return TRUE;
}


static gboolean
do_listen_fwd (SSHSession *session)
{
	GSQL_TRACE_FUNC;
	
	g_return_val_if_fail (session != NULL, FALSE);
	
	int sock = 0, ret, i, n;
	struct addrinfo hints, *ai;
	gboolean wildcard = FALSE, lstatus = FALSE;
	gchar ntop[NI_MAXHOST], strport[NI_MAXSERV];
	GError *err = NULL;

	if (!session)
	{
		g_warning ("do_listen_fwd: 'session' is NULL");
		return lstatus;
	}

	if ((strcmp (session->localname, "0.0.0.0") == 0) ||
	    (strcmp (session->localname, "*") == 0) ||
	    (session->localname == NULL ? 1 :
		     (*session->localname == '\0' ? 1 : 0)) )
	{
		wildcard = TRUE;
	}

	memset (&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC; /* IPv4 or IPv6 */
	hints.ai_flags = wildcard ? AI_PASSIVE : 0;
	hints.ai_socktype = SOCK_STREAM;

	snprintf (strport, sizeof strport, "%d", session->localport);
	
	if (ret = getaddrinfo (session->localname, strport, 
	                       &hints, &ai) != 0)
	{
		g_warning ("do_listen_fwd (getaddrinfo): %s", gai_strerror (ret));
		freeaddrinfo (ai);
		return lstatus;
	}

	if ((ai->ai_family != AF_INET && ai->ai_family != AF_INET6) ||
		(getnameinfo(ai->ai_addr, ai->ai_addrlen, ntop, sizeof(ntop),
			            strport, sizeof(strport), NI_NUMERICHOST|NI_NUMERICSERV) != 0))
	{
		g_warning ("do_listen_fwd (getnameinfo): failed");
		freeaddrinfo (ai);
		return lstatus;
	}

	sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

	if (sock < 0)
	{
		g_warning ("do_listen_fwd (socket): %s",  strerror(errno));
		freeaddrinfo (ai);
		return lstatus;
	}

	i = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i));

	if (bind(sock, ai->ai_addr, ai->ai_addrlen) < 0)
	{
		close (sock);
		freeaddrinfo (ai);
		return lstatus;
	}

	if (listen(sock, 128) < 0)
	{
		g_warning ("do_listen_fwd (listen): %s",  strerror(errno));
		freeaddrinfo (ai);
		close (sock);
		return lstatus;
	}

	g_debug ("create listening socket (%d)... ok", sock);
		
	lstatus = TRUE;
	
	session->sock = sock;

	if (!g_thread_create ((GThreadFunc) do_accept_threaded, session, FALSE, &err))
		g_warning ("cannot create thread");
	
	freeaddrinfo (ai);
	
	return lstatus;
}


static void
do_accept_threaded (gpointer data)
{
	GSQL_TRACE_FUNC;
	
	SSHSession *session = data;
	int s, sock = session->sock; 
	socklen_t addrlen = sizeof(struct sockaddr);
	struct sockaddr addr;
	pid_t chld;

	signal(SIGCHLD, SIG_IGN);

	for (;;)
	{
		
		g_debug ("accepting on socket (%d)", sock);
		
		s = accept (sock, &addr, &addrlen);

		if (s < 0)
		{
			g_warning ("do_accept_threaded (sock=%d): %s", sock, strerror(errno));
			sleep(5);
			continue;
		}

		if ( (chld = fork()) == 0)
		{
			close (session->sock);

			g_warning ("forked!!!!!!!!!!");

			sleep(25);

			close(s);
			_exit(0);
		} 

		close (s);

		
	}	

}

