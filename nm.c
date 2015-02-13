/*
 * Build with: gcc -o nm-adhoc nm.c `pkg-config --libs --cflags gtk+-3.0 libnm-glib libnm-gtk`
 */

#include <gtk/gtk.h>

#include <nm-client.h>
#include <nm-device-wifi.h>
#include <nm-remote-connection.h>
#include <nm-remote-settings.h>
#include <nm-wifi-dialog.h>

NMRemoteSettings *settings;

#define _(x) x

static GSList *
applet_get_all_connections (NMRemoteSettings *settings2)
{
	GSList *connections, *iter, *next;
	NMConnection *connection;
	NMSettingConnection *s_con;

	connections = nm_remote_settings_list_connections (settings);

	/* Ignore slave connections */
	for (iter = connections; iter; iter = next) {
		connection = iter->data;
		next = iter->next;

		s_con = nm_connection_get_setting_connection (connection);
		if (s_con && nm_setting_connection_get_master (s_con))
			connections = g_slist_delete_link (connections, iter);
	}

	return connections;
}

static void
utils_show_error_dialog (const gchar *title,
                         const gchar *text1,
                         const gchar *text2,
                         gboolean     modal,
                         GtkWindow   *parent)
{
	GtkWidget *err_dialog;

	g_return_if_fail (text1 != NULL);

	err_dialog = gtk_message_dialog_new (parent,
	                                     GTK_DIALOG_DESTROY_WITH_PARENT,
	                                     GTK_MESSAGE_ERROR,
	                                     GTK_BUTTONS_CLOSE,
	                                     "%s",
	                                     text1);

	if (text2)
		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (err_dialog), "%s", text2);
	if (title)
		gtk_window_set_title (GTK_WINDOW (err_dialog), title);

	if (modal) {
		gtk_dialog_run (GTK_DIALOG (err_dialog));
		gtk_widget_destroy (err_dialog);
	}
	else {
		g_signal_connect (err_dialog, "delete-event",
		                  G_CALLBACK (gtk_widget_destroy), NULL);
		g_signal_connect (err_dialog, "response",
		                  G_CALLBACK (gtk_widget_destroy), NULL);

		gtk_widget_show_all (err_dialog);
		gtk_window_present (GTK_WINDOW (err_dialog));
	}
}

static void
activate_existing_cb (NMClient           *client,
                      NMActiveConnection *active,
                      GError             *error,
                      gpointer            user_data)
{
	if (error) {
		const char *text = _("Failed to activate connection");
		char *err_text = g_strdup_printf ("(%d) %s", error->code,
		                                  error->message ? error->message : _("Unknown error"));

		g_warning ("%s: %s", text, err_text);
		utils_show_error_dialog (_("Connection failure"), text, err_text, FALSE, NULL);
		g_free (err_text);
	}

	gtk_main_quit();
}

static void
activate_new_cb (NMClient           *client,
                 NMActiveConnection *active,
                 const char         *connection_path,
                 GError             *error,
                 gpointer            user_data)
{
	if (error) {
		const char *text = _("Failed to add new connection");
		char *err_text = g_strdup_printf ("(%d) %s", error->code,
		                                  error->message ? error->message : _("Unknown error"));

		g_warning ("%s: %s", text, err_text);
		utils_show_error_dialog (_("Connection failure"), text, err_text, FALSE, NULL);
		g_free (err_text);
	}
	gtk_main_quit();
}

static void
wifi_dialog_response_cb (GtkDialog *widget,
                         gint       response,
                         gpointer   user_data)
{
	NMAWifiDialog *dialog = NMA_WIFI_DIALOG (widget);
	NMClient *client = NM_CLIENT (user_data);
	NMConnection *connection = NULL, *fuzzy_match = NULL;
	NMDevice *device = NULL;
	NMAccessPoint *ap = NULL;
	GSList *all, *iter;

	if (response != GTK_RESPONSE_OK)
		goto done;

	/* nma_wifi_dialog_get_connection() returns a connection with the
	 * refcount incremented, so the caller must remember to unref it.
	 */

	connection = nma_wifi_dialog_get_connection (dialog, &device, &ap);
	g_assert (connection);
	g_assert (device);

	/* Find a similar connection and use that instead */
	all = applet_get_all_connections (settings);//applet);
	for (iter = all; iter; iter = g_slist_next (iter)) {
		if (nm_connection_compare (connection,
		                           NM_CONNECTION (iter->data),
		                           (NM_SETTING_COMPARE_FLAG_FUZZY | NM_SETTING_COMPARE_FLAG_IGNORE_ID))) {
			fuzzy_match = NM_CONNECTION (iter->data);
			break;
		}
	}
	g_slist_free (all);

	if (fuzzy_match) {
		nm_client_activate_connection (client,
		                               fuzzy_match,
		                               device,
		                               ap ? nm_object_get_path (NM_OBJECT (ap)) : NULL,
		                               activate_existing_cb,
		                               NULL);//applet);
	}
	else {
		NMSetting *s_con;
		NMSettingWireless *s_wifi = NULL;
		const char *mode = NULL;

		/* Entirely new connection */

		/* Don't autoconnect adhoc networks by default for now */
		s_wifi = nm_connection_get_setting_wireless (connection);
		if (s_wifi)
			mode = nm_setting_wireless_get_mode (s_wifi);
		if (g_strcmp0 (mode, "adhoc") == 0 || g_strcmp0 (mode, "ap") == 0) {
			s_con = nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION);
			if (!s_con) {
				s_con = nm_setting_connection_new ();
				nm_connection_add_setting (connection, s_con);
			}
			g_object_set (G_OBJECT (s_con), NM_SETTING_CONNECTION_AUTOCONNECT, FALSE, NULL);
		}

		nm_client_add_and_activate_connection (client,
		                                       connection,
		                                       device,
		                                       ap ? nm_object_get_path (NM_OBJECT (ap)) : NULL,
		                                       activate_new_cb,
		                                       NULL);//applet);
	}

	/* Balance nma_wifi_dialog_get_connection() */
	g_object_unref (connection);

done:
	gtk_widget_hide (GTK_WIDGET (dialog));
	gtk_widget_destroy (GTK_WIDGET (dialog));

	//gtk_main_quit ();
}

static void
show_ignore_focus_stealing_prevention (GtkWidget *widget)
{
	GdkWindow *window;

	gtk_widget_realize (widget);
	gtk_widget_show (widget);
	window = gtk_widget_get_window (widget);
	gtk_window_present_with_time (GTK_WINDOW (widget),
	                              gdk_x11_get_server_time (window));
}

static gboolean
has_usable_wifi (NMClient *client)
{
	const GPtrArray *devices;
	int i;

	if (!nm_client_wireless_get_enabled (client))
		return FALSE;

	devices = nm_client_get_devices (client);
	if (!devices)
		return FALSE;

	for (i = 0; i < devices->len; i++) {
		NMDevice *device = devices->pdata[i];

		if (NM_IS_DEVICE_WIFI (device) &&
		    (nm_device_get_state (device) >= NM_DEVICE_STATE_DISCONNECTED))
			return TRUE;
	}

	return FALSE;
}

int
main (int   argc,
      char *argv[])
{
	NMClient *client = NULL;
	//NMRemoteSettings *settings = NULL;
	DBusGConnection *bus = NULL;
	GtkWidget *dialog = NULL;
	GError *error = NULL;

	gtk_init (&argc, &argv);

	bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (!bus) {
		g_printerr ("Could not get system bus: %s\n", error->message);
		g_error_free (error);
		return -1;
	}

	client = nm_client_new ();
	if (!client)
		return -1;

	if (!has_usable_wifi (client)) {
		g_message("No Wi-Fi devices available.\n");
		g_object_unref (client);
		return 0;
	}

	settings = nm_remote_settings_new (bus);
	dialog = nma_wifi_dialog_new_for_create (client,
	                                         settings);

	if (dialog) {
		g_signal_connect (dialog, "response",
		                  G_CALLBACK (wifi_dialog_response_cb), client);
		//g_signal_connect (dialog, "destroy",
		//                  G_CALLBACK (gtk_main_quit), NULL);

		show_ignore_focus_stealing_prevention (dialog);

		/* Main loop */
		gtk_main ();
	}

	g_object_unref (client);
	g_object_unref (settings);

	dbus_g_connection_unref (bus);

	return 0;
}
