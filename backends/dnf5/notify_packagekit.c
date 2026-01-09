/* Based on https://github.com/rpm-software-management/rpm/blob/master/plugins/dbus_announce.c */

#include <stdlib.h>

#include <dbus/dbus.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmstring.h>
#include <rpm/rpmts.h>
#include <rpm/rpmplugin.h>

struct notify_packagekit_data {
    DBusConnection * bus;
};

static rpmRC notify_packagekit_init(rpmPlugin plugin, rpmts ts)
{
    struct notify_packagekit_data * state = rcalloc(1, sizeof(*state));
    rpmPluginSetData(plugin, state);
    return RPMRC_OK;
}

static void notify_packagekit_close_bus(struct notify_packagekit_data * state)
{
    if (state->bus) {
	dbus_connection_close(state->bus);
	dbus_connection_unref(state->bus);
	state->bus = NULL;
    }
}

static rpmRC open_dbus(rpmPlugin plugin, rpmts ts)
{
    DBusError err;
    struct notify_packagekit_data * state = rpmPluginGetData(plugin);

    /* Already open */
    if (state->bus)
	return RPMRC_OK;

    /* ...don't notify on test transactions */
    if (rpmtsFlags(ts) & (RPMTRANS_FLAG_TEST|RPMTRANS_FLAG_BUILD_PROBS))
	return RPMRC_OK;

    /* ...don't notify on chroot transactions */
    if (!rstreq(rpmtsRootDir(ts), "/"))
	return RPMRC_OK;

    dbus_error_init(&err);

    state->bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
	int ignore = dbus_error_has_name(&err, DBUS_ERROR_NO_SERVER) ||
		     dbus_error_has_name(&err, DBUS_ERROR_FILE_NOT_FOUND);
	rpmlog(ignore ? RPMLOG_DEBUG : RPMLOG_WARNING,
	       "notify_packagekit plugin: Error connecting to dbus (%s)\n",
	       err.message);
	dbus_error_free(&err);
	return RPMRC_OK;
    }

    return RPMRC_OK;
}

static void notify_packagekit_cleanup(rpmPlugin plugin)
{
    struct notify_packagekit_data * state = rpmPluginGetData(plugin);
    notify_packagekit_close_bus(state);
    free(state);
}

static rpmRC notify_packagekit_tsm_post(rpmPlugin plugin, rpmts ts, int res)
{
    struct notify_packagekit_data * state = rpmPluginGetData(plugin);
    DBusMessage* msg = NULL;
    const char * arg = "posttrans";

    if (!state->bus)
	return RPMRC_OK;

    msg = dbus_message_new_method_call("org.freedesktop.PackageKit",  /* service */
				       "/org/freedesktop/PackageKit", /* object path */
				       "org.freedesktop.PackageKit",  /* interface */
				       "StateHasChanged");            /* method */
    if (msg == NULL) {
	rpmlog(RPMLOG_WARNING,
	       "notify_packagekit plugin: Error creating method call message\n");
	return RPMRC_OK;
    }

    if (!dbus_message_append_args(msg,
				  DBUS_TYPE_STRING, &arg,
				  DBUS_TYPE_INVALID)) {
	rpmlog(RPMLOG_WARNING,
	       "notify_packagekit plugin: Error setting message args\n");
	dbus_message_unref(msg);
	return RPMRC_OK;
    }

    if (!dbus_connection_send(state->bus, msg, NULL)) {
	rpmlog(RPMLOG_WARNING,
	       "notify_packagekit plugin: Error sending message\n");
    } else {
	dbus_connection_flush(state->bus);
    }

    dbus_message_unref(msg);

    return RPMRC_OK;
}

static rpmRC notify_packagekit_tsm_pre(rpmPlugin plugin, rpmts ts)
{
    return open_dbus(plugin, ts);
}

struct rpmPluginHooks_s notify_packagekit_hooks = {
    .init = notify_packagekit_init,
    .cleanup = notify_packagekit_cleanup,
    .tsm_pre = notify_packagekit_tsm_pre,
    .tsm_post = notify_packagekit_tsm_post,
};
