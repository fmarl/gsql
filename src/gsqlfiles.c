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


#include <config.h>
#include <string.h>
#include <gtksourceview/gtksourceview.h>
#include <gtksourceview/gtksourcebuffer.h>
#include <libgsql/common.h>
#include <libgsql/stock.h>
#include <libgsql/session.h>
#include <libgsql/workspace.h>
#include <libgsql/sqleditor.h>
#include <libgsql/editor.h>
#include <libgsql/content.h>
#include <libgsql/menu.h>

#include "gsqlmenucb.h"

#define GSQL_DIALOGS_UI PACKAGE_UI_DIR"/gsql_dialogs.ui"

static GtkActionEntry gsqlfiles_action[] = 
{
	{ "ActionFileNew", GTK_STOCK_NEW, N_("New"), "<control>N", N_("New SQL"), G_CALLBACK(on_file_new_sql_activate) },
	{ "ActionFileOpen", GTK_STOCK_OPEN, N_("Open"), "<control>O", N_("Open file"), G_CALLBACK(on_file_open_activate) },
	{ "ActionFileSave", GTK_STOCK_SAVE, N_("Save"), "<control>S", N_("Save"), G_CALLBACK(on_file_save_activate) },
	{ "ActionFileSaveAs", GTK_STOCK_SAVE_AS, N_("Save As..."), NULL, N_("Save file as..."), G_CALLBACK(on_file_save_as_activate) },
	{ "ActionFileClose", GTK_STOCK_CLOSE, N_("Close"), "<control>W", N_("Close"), G_CALLBACK(on_file_close_activate) },
	{ "ActionFileCloseAll", NULL, N_("Close All"), NULL, N_("Close All"), G_CALLBACK(on_file_close_all_activate) },
	{ "ActionFileReload", GTK_STOCK_REVERT_TO_SAVED, N_("Reload"), NULL, N_("Reload"), G_CALLBACK(on_file_reload_activate) },

};

void
gsql_files_menu_init()
{
	GSQL_TRACE_FUNC;

	GtkActionGroup *action;

	action = gtk_action_group_new ("ActionsMenuFiles");
	
	gtk_action_group_add_actions (action, gsqlfiles_action, 
								  G_N_ELEMENTS (gsqlfiles_action), NULL);
	
	gsql_menu_merge_action (action);

}


void
gsql_files_open_file (GSQLSession *session, gchar *file, gchar *encoding)
{
	GSQL_TRACE_FUNC;

	GSQLContent *content;
	GSQLWorkspace *workspace;
	GtkWidget *source;
	GSQLEditor *editor;
	GIOChannel *ioc;
	GtkTextIter iter;
#define BUFFER_SIZE 4096
	gchar buffer[BUFFER_SIZE];
	GtkSourceBuffer *sbuffer;
	GError *err = NULL;
	gchar msg[GSQL_MESSAGE_LEN];
	gboolean reading = TRUE;
	gchar *file_disp;
	GValue bvalue = { 0 };
	
	
	GSQL_DEBUG ("New Editor Activate with: file[%s], encoding[%s]", file, encoding);
	
	workspace = gsql_session_get_workspace (session);
	
	source = (GtkWidget *) gsql_source_editor_new (NULL);
	sbuffer = (GtkSourceBuffer *) gtk_text_view_get_buffer (GTK_TEXT_VIEW (source));
	
	if (file)
	{
		ioc = g_io_channel_new_file (file, "r+", &err);
	
		if (encoding)
			g_io_channel_set_encoding (ioc, encoding, &err);
	
		if (!ioc)
		{
			memset (msg, 0, GSQL_MESSAGE_LEN);
			g_snprintf (msg, GSQL_MESSAGE_LEN, N_("Failed to load file '%s'. %s"), 
					file, err->message);
			gsql_message_add (workspace, GSQL_MESSAGE_ERROR, msg);
			gtk_widget_destroy (source);
			return;
		}
		
		
		gtk_source_buffer_begin_not_undoable_action (sbuffer);
		
		memset (buffer, 0, BUFFER_SIZE);
		
		while (reading)
		{
			gsize bytes_read;
			GIOStatus status;
			
			status = g_io_channel_read_chars (ioc, buffer,
											  BUFFER_SIZE, &bytes_read,
											  &err);
			switch (status)
			{
				case G_IO_STATUS_EOF:
					GSQL_DEBUG ("Opening file: G_IO_STATUS_EOF");
					reading = FALSE;
					break;
					
				case G_IO_STATUS_NORMAL:
					GSQL_DEBUG ("Opening file: G_IO_STATUS_NORMAL");
					if (bytes_read == 0)
						continue;
					
					gtk_text_buffer_get_end_iter (GTK_TEXT_BUFFER (sbuffer), 
												  &iter);
					gtk_text_buffer_insert (GTK_TEXT_BUFFER (sbuffer),
											&iter, buffer, bytes_read);
					break;
					
				case G_IO_STATUS_AGAIN:
					GSQL_DEBUG ("Opening file: G_IO_STATUS_AGAIN");
					continue;
					
				case G_IO_STATUS_ERROR:
				default:
					GSQL_DEBUG ("Opening file: G_IO_STATUS_ERROR");
					
					memset (msg, 0, GSQL_MESSAGE_LEN);
					g_snprintf (msg, GSQL_MESSAGE_LEN, N_("Failed to load file '%s'. %s"), 
								file, err->message);
					gsql_message_add (workspace, GSQL_MESSAGE_ERROR, msg);
					
					gtk_widget_destroy (source);
					g_io_channel_unref (ioc);
					
					return;
			}
		}
		
		g_io_channel_unref (ioc);
		gtk_source_buffer_end_not_undoable_action (sbuffer);
	}

	gtk_text_buffer_set_modified (GTK_TEXT_BUFFER (sbuffer), FALSE);
	gtk_text_buffer_get_start_iter (GTK_TEXT_BUFFER (sbuffer), &iter);
	gtk_text_buffer_place_cursor (GTK_TEXT_BUFFER (sbuffer), &iter);
	
	editor = gsql_editor_new (session, source);
	
	content = gsql_content_new (session, GTK_STOCK_FILE);
	gsql_content_set_child (content, GTK_WIDGET (editor));
	
	workspace = gsql_session_get_workspace (session);
	gsql_workspace_add_content (workspace, content);
	
	if (file)
	{
		file_disp = g_filename_display_basename (file);
		gsql_content_set_name_full (content, file, file_disp);
		g_free (file_disp);
		
		g_value_init(&bvalue, G_TYPE_BOOLEAN);
		g_value_set_boolean (&bvalue, TRUE);
		g_object_set_property (G_OBJECT (editor), "is-file",
						   &bvalue);

	}
	else
		gsql_content_set_name_full (content, NULL, NULL);
	
	gsql_content_set_changed (content, FALSE);
	GSQL_DEBUG ("OK!!!!");
	gtk_window_set_focus (GTK_WINDOW (gsql_window), source);

}
