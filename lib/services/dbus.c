/*
 * Copyright (C) 2014-2016 Andrew Beekhof <andrew@beekhof.net>
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>
#include <crm/crm.h>
#include <crm/services.h>
#include <dbus/dbus.h>
#include <pcmk-dbus.h>

#define BUS_PROPERTY_IFACE "org.freedesktop.DBus.Properties"

static GList *conn_dispatches = NULL;

struct db_getall_data {
    char *name;
    char *target;
    char *object;
    void *userdata;
    void (*callback)(const char *name, const char *value, void *userdata);
};

DBusConnection *
pcmk_dbus_connect(void)
{
    DBusError err;
    DBusConnection *connection;

    dbus_error_init(&err);
    connection = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        crm_err("Could not connect to System DBus: %s", err.message);
        dbus_error_free(&err);
        return NULL;
    }
    if(connection) {
        pcmk_dbus_connection_setup_with_select(connection);
    }
    return connection;
}

void
pcmk_dbus_disconnect(DBusConnection *connection)
{
    return;
}

/*!
 * \internal
 * \brief Check whether a DBus reply indicates an error occurred
 *
 * \param[in]  pending If non-NULL, indicates that a DBus request was sent
 * \param[in]  reply   Reply received from DBus
 * \param[out] ret     If non-NULL, will be set to DBus error, if any
 *
 * \return TRUE if an error was found, FALSE otherwise
 *
 * \note Following the DBus API convention, a TRUE return is exactly equivalent
 *       to ret being set. If ret is provided and this function returns TRUE,
 *       the caller is responsible for calling dbus_error_free() on ret when
 *       done using it.
 */
bool
pcmk_dbus_find_error(DBusPendingCall *pending, DBusMessage *reply,
                     DBusError *ret)
{
    DBusError error;

    dbus_error_init(&error);

    if(pending == NULL) {
        dbus_set_error_const(&error, "org.clusterlabs.pacemaker.NoRequest",
                             "No request sent");

    } else if(reply == NULL) {
        dbus_set_error_const(&error, "org.clusterlabs.pacemaker.NoReply",
                             "No reply");

    } else {
        DBusMessageIter args;
        int dtype = dbus_message_get_type(reply);
        char *sig;

        switch(dtype) {
            case DBUS_MESSAGE_TYPE_METHOD_RETURN:
                dbus_message_iter_init(reply, &args);
                sig = dbus_message_iter_get_signature(&args);
                crm_trace("DBus call returned output args '%s'", sig);
                dbus_free(sig);
                break;
            case DBUS_MESSAGE_TYPE_INVALID:
                dbus_set_error_const(&error,
                                     "org.clusterlabs.pacemaker.InvalidReply",
                                     "Invalid reply");
                break;
            case DBUS_MESSAGE_TYPE_METHOD_CALL:
                dbus_set_error_const(&error,
                                     "org.clusterlabs.pacemaker.InvalidReply.Method",
                                     "Invalid reply (method call)");
                break;
            case DBUS_MESSAGE_TYPE_SIGNAL:
                dbus_set_error_const(&error,
                                     "org.clusterlabs.pacemaker.InvalidReply.Signal",
                                     "Invalid reply (signal)");
                break;
            case DBUS_MESSAGE_TYPE_ERROR:
                dbus_set_error_from_message(&error, reply);
                break;
            default:
                dbus_set_error(&error,
                               "org.clusterlabs.pacemaker.InvalidReply.Type",
                               "Unknown reply type %d", dtype);
        }
    }

    if (dbus_error_is_set(&error)) {
        crm_trace("DBus reply indicated error '%s' (%s)",
                  error.name, error.message);
        if (ret) {
            dbus_error_init(ret);
            dbus_move_error(&error, ret);
        } else {
            dbus_error_free(&error);
        }
        return TRUE;
    }

    return FALSE;
}

/*!
 * \internal
 * \brief Send a DBus request and wait for the reply
 *
 * \param[in]  msg         DBus request to send
 * \param[in]  connection  DBus connection to use
 * \param[out] error       If non-NULL, will be set to error, if any
 * \param[in]  timeout     Timeout to use for request
 *
 * \return DBus reply
 *
 * \note If error is non-NULL, it is initialized, so the caller may always use
 *       dbus_error_is_set() to determine whether an error occurred; the caller
 *       is responsible for calling dbus_error_free() in this case.
 */
DBusMessage *
pcmk_dbus_send_recv(DBusMessage *msg, DBusConnection *connection,
                    DBusError *error, int timeout)
{
    const char *method = NULL;
    DBusMessage *reply = NULL;
    DBusPendingCall* pending = NULL;

    CRM_ASSERT(dbus_message_get_type (msg) == DBUS_MESSAGE_TYPE_METHOD_CALL);
    method = dbus_message_get_member (msg);

    /* Ensure caller can reliably check whether error is set */
    if (error) {
        dbus_error_init(error);
    }

    if (timeout <= 0) {
        /* DBUS_TIMEOUT_USE_DEFAULT (-1) tells DBus to use a sane default */
        timeout = DBUS_TIMEOUT_USE_DEFAULT;
    }

    // send message and get a handle for a reply
    if (!dbus_connection_send_with_reply(connection, msg, &pending, timeout)) {
        if(error) {
            dbus_set_error(error, "org.clusterlabs.pacemaker.SendFailed",
                           "Could not queue DBus '%s' request", method);
        }
        return NULL;
    }

    dbus_connection_flush(connection);

    if(pending) {
        /* block until we receive a reply */
        dbus_pending_call_block(pending);

        /* get the reply message */
        reply = dbus_pending_call_steal_reply(pending);
    }

    (void)pcmk_dbus_find_error(pending, reply, error);

    if(pending) {
        /* free the pending message handle */
        dbus_pending_call_unref(pending);
    }

    return reply;
}

DBusPendingCall *
pcmk_dbus_send(DBusMessage *msg, DBusConnection *connection,
               void(*done)(DBusPendingCall *pending, void *user_data),
               void *user_data, int timeout)
{
    const char *method = NULL;
    DBusPendingCall* pending = NULL;

    CRM_ASSERT(done);
    CRM_ASSERT(dbus_message_get_type (msg) == DBUS_MESSAGE_TYPE_METHOD_CALL);
    method = dbus_message_get_member (msg);


    if (timeout <= 0) {
        /* DBUS_TIMEOUT_USE_DEFAULT (-1) tells DBus to use a sane default */
        timeout = DBUS_TIMEOUT_USE_DEFAULT;
    }

    // send message and get a handle for a reply
    if (!dbus_connection_send_with_reply(connection, msg, &pending, timeout)) {
        crm_err("Send with reply failed for %s", method);
        return NULL;

    } else if (pending == NULL) {
        crm_err("No pending call found for %s: Connection to System DBus may be closed", method);
        return NULL;
    }

    crm_trace("DBus %s call sent", method);
    if (dbus_pending_call_get_completed(pending)) {
        crm_info("DBus %s call completed too soon", method);
        if(done) {
#if 0
            /* This sounds like a good idea, but allegedly it breaks things */
            done(pending, user_data);
            pending = NULL;
#else
            CRM_ASSERT(dbus_pending_call_set_notify(pending, done, user_data, NULL));
#endif
        }

    } else if(done) {
        CRM_ASSERT(dbus_pending_call_set_notify(pending, done, user_data, NULL));
    }
    return pending;
}

bool
pcmk_dbus_type_check(DBusMessage *msg, DBusMessageIter *field, int expected,
                     const char *function, int line)
{
    int dtype = 0;
    DBusMessageIter lfield;

    if(field == NULL) {
        if(dbus_message_iter_init(msg, &lfield)) {
            field = &lfield;
        }
    }

    if(field == NULL) {
        do_crm_log_alias(LOG_ERR, __FILE__, function, line,
                         "Empty parameter list in reply expecting '%c'", expected);
        return FALSE;
    }

    dtype = dbus_message_iter_get_arg_type(field);

    if(dtype != expected) {
        DBusMessageIter args;
        char *sig;

        dbus_message_iter_init(msg, &args);
        sig = dbus_message_iter_get_signature(&args);
        do_crm_log_alias(LOG_ERR, __FILE__, function, line,
                         "Unexpected DBus type, expected %c in '%s' instead of %c",
                         expected, sig, dtype);
        dbus_free(sig);
        return FALSE;
    }

    return TRUE;
}

static char *
pcmk_dbus_lookup_result(DBusMessage *reply, struct db_getall_data *data)
{
    DBusError error;
    char *output = NULL;
    DBusMessageIter dict;
    DBusMessageIter args;

    if (pcmk_dbus_find_error((void*)&error, reply, &error)) {
        crm_err("Cannot get properties from %s for %s: %s",
                data->target, data->object, error.message);
        dbus_error_free(&error);
        goto cleanup;
    }

    dbus_message_iter_init(reply, &args);
    if(!pcmk_dbus_type_check(reply, &args, DBUS_TYPE_ARRAY, __FUNCTION__, __LINE__)) {
        crm_err("Invalid reply from %s for %s", data->target, data->object);
        goto cleanup;
    }

    dbus_message_iter_recurse(&args, &dict);
    while (dbus_message_iter_get_arg_type (&dict) != DBUS_TYPE_INVALID) {
        DBusMessageIter sv;
        DBusMessageIter v;
        DBusBasicValue name;
        DBusBasicValue value;

        if(!pcmk_dbus_type_check(reply, &dict, DBUS_TYPE_DICT_ENTRY, __FUNCTION__, __LINE__)) {
            dbus_message_iter_next (&dict);
            continue;
        }

        dbus_message_iter_recurse(&dict, &sv);
        while (dbus_message_iter_get_arg_type (&sv) != DBUS_TYPE_INVALID) {
            int dtype = dbus_message_iter_get_arg_type(&sv);

            switch(dtype) {
                case DBUS_TYPE_STRING:
                    dbus_message_iter_get_basic(&sv, &name);

                    if(data->name && strcmp(name.str, data->name) != 0) {
                        dbus_message_iter_next (&sv); /* Skip the value */
                    }
                    break;
                case DBUS_TYPE_VARIANT:
                    dbus_message_iter_recurse(&sv, &v);
                    if(pcmk_dbus_type_check(reply, &v, DBUS_TYPE_STRING, __FUNCTION__, __LINE__)) {
                        dbus_message_iter_get_basic(&v, &value);

                        crm_trace("Property %s[%s] is '%s'", data->object, name.str, value.str);
                        if(data->callback) {
                            data->callback(name.str, value.str, data->userdata);

                        } else {
                            free(output);
                            output = strdup(value.str);
                        }

                        if(data->name) {
                            goto cleanup;
                        }
                    }
                    break;
                default:
                    pcmk_dbus_type_check(reply, &sv, DBUS_TYPE_STRING, __FUNCTION__, __LINE__);
            }
            dbus_message_iter_next (&sv);
        }

        dbus_message_iter_next (&dict);
    }

    if(data->name && data->callback) {
        crm_trace("No value for property %s[%s]", data->object, data->name);
        data->callback(data->name, NULL, data->userdata);
    }

  cleanup:
    free(data->target);
    free(data->object);
    free(data->name);
    free(data);

    return output;
}

static void
pcmk_dbus_lookup_cb(DBusPendingCall *pending, void *user_data)
{
    DBusMessage *reply = NULL;
    char *value = NULL;

    if(pending) {
        reply = dbus_pending_call_steal_reply(pending);
    }

    value = pcmk_dbus_lookup_result(reply, user_data);
    free(value);

    if(reply) {
        dbus_message_unref(reply);
    }
}

char *
pcmk_dbus_get_property(DBusConnection *connection, const char *target,
                       const char *obj, const gchar * iface, const char *name,
                       void (*callback)(const char *name, const char *value, void *userdata),
                       void *userdata, DBusPendingCall **pending, int timeout)
{
    DBusMessage *msg;
    const char *method = "GetAll";
    char *output = NULL;
    struct db_getall_data *query_data = NULL;

    crm_debug("Calling: %s on %s", method, target);
    msg = dbus_message_new_method_call(target, // target for the method call
                                       obj, // object to call on
                                       BUS_PROPERTY_IFACE, // interface to call on
                                       method); // method name
    if (NULL == msg) {
        crm_err("Call to %s failed: No message", method);
        return NULL;
    }

    CRM_LOG_ASSERT(dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID));

    query_data = malloc(sizeof(struct db_getall_data));
    if(query_data == NULL) {
        crm_err("Call to %s failed: malloc failed", method);
        return NULL;
    }

    query_data->target = strdup(target);
    query_data->object = strdup(obj);
    query_data->callback = callback;
    query_data->userdata = userdata;
    query_data->name = NULL;

    if(name) {
        query_data->name = strdup(name);
    }

    if(query_data->callback) {
        DBusPendingCall* _pending;
        _pending = pcmk_dbus_send(msg, connection, pcmk_dbus_lookup_cb, query_data, timeout);
        if (pending != NULL) {
            *pending = _pending;
        }

    } else {
        DBusMessage *reply = pcmk_dbus_send_recv(msg, connection, NULL, timeout);

        output = pcmk_dbus_lookup_result(reply, query_data);

        if(reply) {
            dbus_message_unref(reply);
        }
    }

    dbus_message_unref(msg);

    return output;
}

static void
pcmk_dbus_connection_dispatch_status(DBusConnection *connection,
                              DBusDispatchStatus new_status, void *data)
{
    crm_trace("New status %d for connection %p", new_status, connection);
    if (new_status == DBUS_DISPATCH_DATA_REMAINS){
        conn_dispatches = g_list_prepend(conn_dispatches, connection);
    }
}

static void
pcmk_dbus_connections_dispatch()
{
    GList *gIter = NULL;

    for (gIter = conn_dispatches; gIter != NULL; gIter = gIter->next) {
        DBusConnection *connection = gIter->data;

        while (dbus_connection_get_dispatch_status(connection) == DBUS_DISPATCH_DATA_REMAINS) {
            crm_trace("Dispatching for connection %p", connection);
            dbus_connection_dispatch(connection);
        }
    }

    g_list_free(conn_dispatches);
    conn_dispatches = NULL;
}

/* Copied from dbus-watch.c */

static const char*
dbus_watch_flags_to_string(int flags)
{
    const char *watch_type;

    if ((flags & DBUS_WATCH_READABLE) && (flags & DBUS_WATCH_WRITABLE)) {
        watch_type = "readwrite";
    } else if (flags & DBUS_WATCH_READABLE) {
        watch_type = "read";
    } else if (flags & DBUS_WATCH_WRITABLE) {
        watch_type = "write";
    } else {
        watch_type = "not read or write";
    }
    return watch_type;
}

static int
pcmk_dbus_watch_dispatch(gpointer userdata)
{
    bool oom = FALSE;
    DBusWatch *watch = userdata;
    int flags = dbus_watch_get_flags(watch);
    bool enabled = dbus_watch_get_enabled (watch);
    mainloop_io_t *client = dbus_watch_get_data(watch);

    crm_trace("Dispatching client %p: %s", client, dbus_watch_flags_to_string(flags));
    if (enabled && (flags & (DBUS_WATCH_READABLE|DBUS_WATCH_WRITABLE))) {
        oom = !dbus_watch_handle(watch, flags);

    } else if(enabled) {
        oom = !dbus_watch_handle(watch, DBUS_WATCH_ERROR);
    }

    if(flags != dbus_watch_get_flags(watch)) {
        flags = dbus_watch_get_flags(watch);
        crm_trace("Dispatched client %p: %s (%d)", client,
                  dbus_watch_flags_to_string(flags), flags);
    }

    if(oom) {
        crm_err("DBus encountered OOM while attempting to dispatch %p (%s)",
                client, dbus_watch_flags_to_string(flags));

    } else {
        pcmk_dbus_connections_dispatch();
    }

    return 0;
}

static void
pcmk_dbus_watch_destroy(gpointer userdata)
{
    mainloop_io_t *client = dbus_watch_get_data(userdata);
    crm_trace("Destroyed %p", client);
}


struct mainloop_fd_callbacks pcmk_dbus_cb = {
    .dispatch = pcmk_dbus_watch_dispatch,
    .destroy = pcmk_dbus_watch_destroy,
};

static dbus_bool_t
pcmk_dbus_watch_add(DBusWatch *watch, void *data)
{
    int fd = dbus_watch_get_unix_fd(watch);

    mainloop_io_t *client = mainloop_add_fd(
        "dbus", G_PRIORITY_DEFAULT, fd, watch, &pcmk_dbus_cb);

    crm_trace("Added watch %p with fd=%d to client %p", watch, fd, client);
    dbus_watch_set_data(watch, client, NULL);
    return TRUE;
}

static void
pcmk_dbus_watch_toggle(DBusWatch *watch, void *data)
{
    mainloop_io_t *client = dbus_watch_get_data(watch);
    crm_notice("DBus client %p is now %s",
               client, (dbus_watch_get_enabled(watch)? "enabled" : "disabled"));
}


static void
pcmk_dbus_watch_remove(DBusWatch *watch, void *data)
{
    mainloop_io_t *client = dbus_watch_get_data(watch);

    crm_trace("Removed client %p (%p)", client, data);
    mainloop_del_fd(client);
}

static gboolean
pcmk_dbus_timeout_dispatch(gpointer data)
{
    crm_info("Timeout %p expired", data);
    dbus_timeout_handle(data);
    return FALSE;
}

static dbus_bool_t
pcmk_dbus_timeout_add(DBusTimeout *timeout, void *data)
{
    guint id = g_timeout_add(dbus_timeout_get_interval(timeout),
                             pcmk_dbus_timeout_dispatch, timeout);

    crm_trace("Adding timeout %p (%d)", timeout, dbus_timeout_get_interval(timeout));

    if(id) {
        dbus_timeout_set_data(timeout, GUINT_TO_POINTER(id), NULL);
    }
    return TRUE;
}

static void
pcmk_dbus_timeout_remove(DBusTimeout *timeout, void *data)
{
    void *vid = dbus_timeout_get_data(timeout);
    guint id = GPOINTER_TO_UINT(vid);

    crm_trace("Removing timeout %p (%p)", timeout, data);

    if(id) {
        g_source_remove(id);
        dbus_timeout_set_data(timeout, 0, NULL);
    }
}

static void
pcmk_dbus_timeout_toggle(DBusTimeout *timeout, void *data)
{
    bool enabled = dbus_timeout_get_enabled(timeout);

    crm_trace("Toggling timeout for %p to %s", timeout, enabled?"off":"on");

    if(enabled) {
        pcmk_dbus_timeout_add(timeout, data);
    } else {
        pcmk_dbus_timeout_remove(timeout, data);
    }
}

/* Inspired by http://www.kolej.mff.cuni.cz/~vesej3am/devel/dbus-select.c */

void
pcmk_dbus_connection_setup_with_select(DBusConnection *c)
{
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    dbus_connection_set_timeout_functions(c, pcmk_dbus_timeout_add,
                                          pcmk_dbus_timeout_remove,
                                          pcmk_dbus_timeout_toggle, NULL, NULL);
    dbus_connection_set_watch_functions(c, pcmk_dbus_watch_add,
                                        pcmk_dbus_watch_remove,
                                        pcmk_dbus_watch_toggle, NULL, NULL);
    dbus_connection_set_dispatch_status_function(c, pcmk_dbus_connection_dispatch_status, NULL, NULL);
    pcmk_dbus_connection_dispatch_status(c, dbus_connection_get_dispatch_status(c), NULL);
}
