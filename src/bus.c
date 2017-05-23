/*
 * Copyright 2016, Grinn
 *
 * SPDX-License-Identifier: MIT
 */

#include "bus.h"

#include "compat.h"
#include "easydbus.h"
#include "poll.h"
#include "utils.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>

static int bus_mt;
#define BUS_MT ((void *) &bus_mt)

static int watch_mt;
#define WATCH_MT ((void *) &watch_mt)

static int timeout_mt;
#define TIMEOUT_MT ((void *) &timeout_mt)

static DBusConnection *get_conn(lua_State *L, int index)
{
    DBusConnection *conn;

    lua_rawgeti(L, index, 1);

    conn = lua_touserdata(L, -1);
    lua_pop(L, 1);

    return conn;
}

static void dump_arg(lua_State *L, int print_index, int real_index)
{
    if (lua_type(L, real_index) == LUA_TSTRING)
        g_debug("arg %d: %s", print_index, lua_tostring(L, real_index));
    else
        g_debug("arg %d: type=%s", print_index,
                lua_typename(L, lua_type(L, real_index)));
}

static void dump_args(lua_State *L, int start_index, int stop_index)
{
    int i;

    for (i = start_index; i <= stop_index; i++)
        dump_arg(L, i, i);
}

static void unpack_table(lua_State *L, int table_index,
                         int start_index, int stop_index)
{
    int i;

    for (i = start_index; i <= stop_index; i++) {
        lua_rawgeti(L, table_index, i);

        dump_arg(L, i, -1);
    }
}

static void call_callback(DBusPendingCall *pending_call, void *data)
{
    lua_State *T = data;
    //DBusConnection *conn = lua_touserdata(T, 1);
    int n_args = lua_gettop(T);
    DBusMessage *msg = dbus_pending_call_steal_reply(pending_call);
    DBusError error;
    assert(msg);

    g_debug("call_callback(%p)", data);
    dump_args(T, 1, n_args);

    dbus_pending_call_unref(pending_call);

    if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
        g_debug("got reply");

        /* Resume Lua callback */
        ed_resume(T, 1 + push_msg(T, msg));
    } else {
        dbus_error_init(&error);
        dbus_set_error_from_message(&error, msg);
        lua_pushnil(T);
        lua_pushstring(T, error.name);
        lua_pushstring(T, error.message);
        dbus_error_free(&error);
        ed_resume(T, 4);
    }

    dbus_message_unref(msg);

    /* Remove thread from registry, so garbage collection can take place */
    /*
     * TODO: do not remove thread from itself, do it from main thread
     */
#if 0
    lua_pushlightuserdata(T, T);
    lua_pushnil(T);
    lua_rawset(T, LUA_REGISTRYINDEX);
#endif
}

static void notify_delete(void *data)
{
    g_debug("notify_delete %p", data);
}

/*
 * Args:
 * 1) conn
 * 2) bus_name
 * 3) object_path
 * 4) interface_name
 * 5) method_name
 * 6) parameters ...
 * last-2) timeout
 * last-1) callback
 * last) callback_arg
 */
static int bus_call(lua_State *L)
{
    struct easydbus_state *state = lua_touserdata(L, lua_upvalueindex(1));
    DBusConnection *conn = get_conn(L, 1);
    const char *dest = luaL_checkstring(L, 2);
    const char *object_path = luaL_checkstring(L, 3);
    const char *interface_name = luaL_checkstring(L, 4);
    const char *method_name = luaL_checkstring(L, 5);
    const char *sig = NULL;
    int timeout = DBUS_TIMEOUT_USE_DEFAULT;
    int opt_type = lua_type(L, 6);
    lua_State *T;
    int i, n_args = lua_gettop(L);
    int n_params = n_args - 6;
    DBusMessage *msg;
    DBusPendingCall *pending_call;
    dbus_bool_t ret;
    DBusError error;

    g_debug("%s: conn=%p dest=%s object_path=%s interface_name=%s method_name=%s sig=%s",
            __FUNCTION__, (void *) conn, dest, object_path, interface_name, method_name, sig);
    dump_args(L, 2, n_args);

    luaL_argcheck(L, g_dbus_is_name(dest), 2, "Invalid bus name");
    luaL_argcheck(L, g_variant_is_object_path(object_path), 3, "Invalid object path");
    luaL_argcheck(L, g_dbus_is_interface_name(interface_name), 4, "Invalid interface name");

    switch (opt_type) {
    case LUA_TSTRING:
        sig = lua_tostring(L, 6);
        break;
    case LUA_TTABLE:
        lua_rawgeti(L, 6, 1);
        sig = lua_tostring(L, -1);
        lua_pushliteral(L, "timeout");
        lua_rawget(L, 6);
        if (lua_isnumber(L, -1))
            timeout = round(lua_tonumber(L, -1) * 1000);
        lua_pop(L, 2);
        break;
    default:
        break;
    }

    msg = dbus_message_new_method_call(dest, object_path, interface_name,
                                       method_name);
    assert(msg);

    if (L == state->L) {
        DBusMessage *result;
        int ret;

        if (n_params > 0)
            range_to_msg(msg, L, 7, 7 + n_params, sig);

        dbus_error_init(&error);
        result = dbus_connection_send_with_reply_and_block(conn, msg, timeout, &error);

        if (!result) {
            lua_pushnil(L);
            lua_pushstring(L, error.name);
            lua_pushstring(L, error.message);
            dbus_error_free(&error);
            return 3;
        }

        ret = push_msg(L, result);
        dbus_message_unref(result);
        return ret;
    }

    g_debug("Out of mainloop");

    /* Remove callback + user_data */
    n_params -= 2;

    T = lua_newthread(L);

    lua_pushlightuserdata(L, conn);
    for (i = 2; i <= n_args; i++)
        lua_pushvalue(L, i);
    lua_xmove(L, T, n_args);

    /* Push thread to registry so we will prevent garbage collection */
    lua_pushlightuserdata(L, T);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    /* Read parameters */
    if (n_params > 0)
        range_to_msg(msg, L, 7, 7 + n_params, sig);

    ret = dbus_connection_send_with_reply(conn, msg, &pending_call, timeout);
    assert(ret);

    g_debug("set_notify");
    ret = dbus_pending_call_set_notify(pending_call, call_callback, T, notify_delete);
    assert(ret);

    return 0;
}

/*
 * Args:
 * 1) invocation method
 */
static int interface_method_return(lua_State *L)
{
    DBusConnection *conn;
    DBusMessage *msg;
    int n_args = lua_gettop(L);
    const char *out_sig;
    DBusMessage *reply;
    dbus_bool_t ret;

    luaL_argcheck(L, lua_istable(L, 1), 1, "table expected");
    lua_rawgeti(L, 1, 1);
    lua_rawgeti(L, 1, 2);
    lua_rawgeti(L, 1, 3);
    conn = lua_touserdata(L, -3);
    msg = lua_touserdata(L, -2);
    out_sig = lua_tostring(L, -1);
    lua_pop(L, 3);

    g_debug("%s: sender=%s object_path=%s interface_name=%s method_name=%s out_sig=%s",
            __FUNCTION__,
            dbus_message_get_sender(msg), dbus_message_get_path(msg),
            dbus_message_get_interface(msg), dbus_message_get_member(msg),
            out_sig);
    dump_args(L, 2, n_args);

    /* In case of (nil, error_msg) return DBus error */
    if (lua_isnil(L, 2)) {
        reply = dbus_message_new_error(msg, DBUS_ERROR_FAILED,
                                       luaL_tolstring(L, 3, NULL));
        assert(reply);
        dbus_message_unref(msg);
        dbus_connection_send(conn, reply, NULL);

        return 0;
    }

    reply = dbus_message_new_method_return(msg);
    assert(reply);
    dbus_message_unref(msg);

    range_to_msg(reply, L, 2, n_args + 1, out_sig);
    ret = dbus_connection_send(conn, reply, NULL);
    assert(ret);

    return 0;
}

static DBusHandlerResult interface_method_call(DBusConnection *connection,
                                               DBusMessage *msg,
                                               void *data)
{
    struct easydbus_state *state = data;
    const char *path = dbus_message_get_path(msg);
    const char *interface = dbus_message_get_interface(msg);
    const char *method = dbus_message_get_member(msg);
    lua_State *T;
    int n_args;
    int n_params;
    DBusMessage *reply;

    g_debug("%s: sender=%s object_path=%s interface_name=%s method_name=%s type=%d",
            __FUNCTION__,
            dbus_message_get_sender(msg), path, interface, method, (int) dbus_message_get_type(msg));

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    T = lua_newthread(state->L);

    /* push callback with args */
    lua_pushlightuserdata(T, connection);
    lua_rawget(T, LUA_REGISTRYINDEX);
    lua_rawgeti(T, -1, 2);

    lua_pushstring(T, path);
    lua_rawget(T, -2);
    if (!lua_istable(T, -1)) {
        reply = dbus_message_new_error_printf(msg, DBUS_ERROR_UNKNOWN_OBJECT,
                    "No such object path '%s'",
                    path);
        assert(reply);
        dbus_connection_send(connection, reply, NULL);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    lua_pushstring(T, interface);
    lua_rawget(T, -2);
    if (!lua_istable(T, -1)) {
        reply = dbus_message_new_error_printf(msg, DBUS_ERROR_UNKNOWN_INTERFACE,
                    "No such interface '%s' at object path '%s'",
                    interface, path);
        assert(reply);
        dbus_connection_send(connection, reply, NULL);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    lua_pushstring(T, method);
    lua_rawget(T, -2);
    if (!lua_istable(T, -1)) {
        reply = dbus_message_new_error_printf(msg, DBUS_ERROR_UNKNOWN_METHOD,
                    "No such method '%s' in interface '%s' at object path '%s'",
                    method, interface, path);
        assert(reply);
        dbus_connection_send(connection, reply, NULL);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    n_args = lua_rawlen(T, 5);
    unpack_table(T, 5, 3, n_args);

    dbus_message_ref(msg);

    /* push params */
    n_params = push_msg(T, msg);
    lua_pushcclosure(T, interface_method_return, 0);
    lua_createtable(T, 3, 0);
    lua_pushlightuserdata(T, connection);
    lua_rawseti(T, -2, 1);
    lua_pushlightuserdata(T, msg);
    lua_rawseti(T, -2, 2);
    lua_rawgeti(T, 5, 2); /* out_sig */
    lua_rawseti(T, -2, 3);
    ed_resume(T, n_args + n_params - 1);

    /*
     * TODO: Do not remove thread in case of yield
     */
    lua_pop(state->L, 1);

    return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable interface_vtable = {
    .message_function = interface_method_call,
};

static int bus_emit(lua_State *L)
{
    DBusConnection *conn = get_conn(L, 1);
    DBusMessage *msg;
    const char *listener = lua_tostring(L, 2);
    const char *object_path = luaL_checkstring(L, 3);
    const char *interface_name = luaL_checkstring(L, 4);
    const char *signal_name = luaL_checkstring(L, 5);
    const char *sig = lua_tostring(L, 6);
    dbus_bool_t ret;

    g_debug("%s: listener=%s object_path=%s interface_name=%s signal_name=%s sig=%s",
            __FUNCTION__, listener, object_path, interface_name, signal_name, sig);

    if (listener)
        luaL_argcheck(L, dbus_validate_bus_name(listener, NULL), 2, "Invalid listener name");
    luaL_argcheck(L, dbus_validate_path(object_path, NULL), 3, "Invalid object path");
    luaL_argcheck(L, dbus_validate_interface(interface_name, NULL), 4, "Invalid interface name");

    msg = dbus_message_new_signal(object_path, interface_name, signal_name);
    if (listener) {
        dbus_bool_t ret = dbus_message_set_destination(msg, listener);
        assert(ret);
    }

    range_to_msg(msg, L, 7, lua_gettop(L) + 1, sig);

    ret = dbus_connection_send(conn, msg, NULL);
    dbus_message_unref(msg);

    if (!ret) {
        lua_pushnil(L);
        lua_pushliteral(L, "Out of memory");
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static DBusHandlerResult signal_callback(DBusConnection *conn,
                                         DBusMessage *msg,
                                         void *data)
{
    struct easydbus_state *state = data;
    const char *path = dbus_message_get_path(msg);
    const char *interface = dbus_message_get_interface(msg);
    const char *signal = dbus_message_get_member(msg);
    int n_args;
    lua_State *L;

    g_debug("%s: path=%s interface=%s signal=%s", __FUNCTION__, path, interface, signal);

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    L = lua_newthread(state->L);

    lua_pushlightuserdata(L, conn);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, -1, 3);
    lua_pushfstring(L, "%s:%s:%s", path, interface, signal);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        g_debug("No such handler");
        lua_pop(state->L, 1);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    n_args = lua_rawlen(L, 3);
    unpack_table(L, 3, 1, n_args);

    ed_resume(L, n_args + push_msg(L, msg) - 1);

    lua_pop(state->L, 1);

    return DBUS_HANDLER_RESULT_HANDLED;
}

struct sb {
    char *str;
    size_t len;
    size_t offset;
};

#define SB_STEP 4096

static void sb_init(struct sb *sb)
{
    sb->str = malloc(SB_STEP);
    sb->len = SB_STEP;
    sb->offset = 0;
}

static void sb_free(struct sb *sb)
{
    free(sb->str);
}

static void sb_addstring(struct sb *sb, const char *chunk)
{
    size_t chunk_len = strlen(chunk);

    if (chunk_len + sb->offset > sb->len) {
        sb->len += SB_STEP;
        sb->str = realloc(sb->str, sb->len);
    }

    strcpy(sb->str + sb->offset, chunk);
    sb->offset += chunk_len;
}

static void introspect_handler(DBusConnection *conn,
                               DBusMessage *msg,
                               const char *path,
                               struct easydbus_state *state)
{
    DBusMessage *reply;
    DBusMessageIter msg_iter;
    lua_State *L = state->L;
    int top = lua_gettop(L);
    const char *interface;
    struct sb b;

    lua_pushlightuserdata(L, conn);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, -1, 2);

    lua_pushstring(L, path);
    lua_rawget(L, -2);

    if (!lua_istable(L, -1)) {
        reply = dbus_message_new_error_printf(msg, DBUS_ERROR_UNKNOWN_OBJECT,
                    "No such object path '%s'",
                    path);
        assert(reply);
        dbus_connection_send(conn, reply, NULL);
        return;
    }

    reply = dbus_message_new_method_return(msg);
    dbus_message_iter_init_append(reply, &msg_iter);

    sb_init(&b);
    sb_addstring(&b, "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
                 "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
                 "<node>\n");

    /* Push interfaces */
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }

        interface = lua_tostring(L, -2);

        sb_addstring(&b, "  <interface name=\"");
        sb_addstring(&b, interface);
        sb_addstring(&b, "\">\n");

        lua_pushnil(L);
        while (lua_next(L, -2)) {
            const char *method = lua_tostring(L, -2);
            const char *in_sig;
            const char *out_sig;

            sb_addstring(&b, "    <method name=\"");
            sb_addstring(&b, method);
            sb_addstring(&b, "\">\n");

            lua_rawgeti(L, -1, 1);
            in_sig = lua_tostring(L, -1);
            if (in_sig && in_sig[0] != '\0') {
                sb_addstring(&b, "      <arg type=\"");
                sb_addstring(&b, in_sig);
                sb_addstring(&b, "\" direction=\"in\"/>\n");
            }
            lua_pop(L, 1);

            lua_rawgeti(L, -1, 2);
            out_sig = lua_tostring(L, -1);
            if (out_sig && out_sig[0] != '\0') {
                sb_addstring(&b, "      <arg type=\"");
                sb_addstring(&b, out_sig);
                sb_addstring(&b, "\" direction=\"out\"/>\n");
            }
            lua_pop(L, 1);

            sb_addstring(&b, "    </method>\n");

            lua_pop(L, 1);
        }

        sb_addstring(&b, "  </interface>\n");

        lua_pop(L, 1);
    }

    /* Push standard interfaces */
    sb_addstring(&b,
                 "  <interface name=\"" DBUS_INTERFACE_INTROSPECTABLE "\">\n"
                 "    <method name=\"Introspect\">\n"
                 "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
                 "    </method>\n"
                 "  </interface>\n"
                 "  <interface name=\"" DBUS_INTERFACE_PEER "\">\n"
                 "    <method name=\"Ping\"/>\n"
                 "    <method name=\"GetMachineId\">\n"
                 "      <arg name=\"machine_uuid\" type=\"s\" direction=\"out\"/>\n"
                 "    </method>\n"
                 "  </interface>\n"
        );

    /* Push subnodes */
    lua_rawgeti(L, -1, 1);
    lua_pushnil(L);
    while(lua_next(L, -2)) {
        sb_addstring(&b, "  <node name=\"");
        sb_addstring(&b, lua_tostring(L, -2));
        sb_addstring(&b, "\"/>\n");
        lua_pop(L, 1);
    }

    sb_addstring(&b, "</node>\n");
    dbus_message_iter_append_basic(&msg_iter, DBUS_TYPE_STRING, &b.str);
    sb_free(&b);

    dbus_connection_send(conn, reply, NULL);

    lua_settop(L, top);
}

static DBusHandlerResult standard_methods_callback(DBusConnection *conn,
                                                   DBusMessage *msg,
                                                   void *data)
{
    struct easydbus_state *state = data;
    const char *path = dbus_message_get_path(msg);
    const char *interface = dbus_message_get_interface(msg);
    const char *method = dbus_message_get_member(msg);

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    g_debug("%s: path=%s interface=%s method=%s", __FUNCTION__, path, interface, method);

    if (!strcmp(interface, DBUS_INTERFACE_INTROSPECTABLE)) {
        if (!strcmp(method, "Introspect")) {
            introspect_handler(conn, msg, path, state);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

luaL_Reg bus_funcs[] = {
    {"call", bus_call},
    {"emit", bus_emit},
    {NULL, NULL},
};

static int flags_dbus_to_ev(unsigned int flags)
{
    int events = 0;

    if (flags & DBUS_WATCH_READABLE)
        events |= EV_READ;
    if (flags & DBUS_WATCH_WRITABLE)
        events |= EV_WRITE;

    return events;
}

static unsigned int flags_ev_to_dbus(int events)
{
    unsigned int flags = 0;

    if (events & EV_READ)
        flags |= DBUS_WATCH_READABLE;
    if (events & EV_WRITE)
        flags |= DBUS_WATCH_WRITABLE;

    return flags;
}

struct ev_loop_wrap {
    struct ev_loop *loop;
    struct DBusConnection *conn;
    struct easydbus_state *state;
};

static void io_cb(struct ev_loop *loop, struct ev_io *io, int revents)
{
    struct ev_io_wrap *io_wrap = container_of(io, struct ev_io_wrap, io);
    dbus_bool_t ret;

    g_debug("io_cb %p %d %d", (void *) io_wrap->watch, dbus_watch_get_unix_fd(io_wrap->watch), revents);

    ret = dbus_watch_handle(io_wrap->watch, flags_ev_to_dbus(revents));
    assert(ret);

    g_debug("io_cb dispatch");
    while (dbus_connection_dispatch(io_wrap->conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;

    g_debug("io_cb exit");
}

static void timer_cb(struct ev_loop *loop, struct ev_timer *timer, int revents)
{
    struct ev_timer_wrap *timer_wrap = container_of(timer, struct ev_timer_wrap, timer);

    g_debug("timer_cb %p", (void *) timer_wrap->timeout);

    assert(dbus_timeout_handle(timer_wrap->timeout));

    while (dbus_connection_dispatch(timer_wrap->conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;
}

static int watch_fd(lua_State *L)
{
    struct ev_io_wrap *io = lua_touserdata(L, 1);
    lua_pushinteger(L, dbus_watch_get_unix_fd(io->watch));
    return 1;
}

static int watch_flags(lua_State *L)
{
    struct ev_io_wrap *io = lua_touserdata(L, 1);
    lua_pushinteger(L, dbus_watch_get_flags(io->watch));
    return 1;
}

static int watch_enabled(lua_State *L)
{
    struct ev_io_wrap *io = lua_touserdata(L, 1);
    lua_pushboolean(L, dbus_watch_get_enabled(io->watch));
    return 1;
}

static int watch_handle(lua_State *L)
{
    struct ev_io_wrap *io = lua_touserdata(L, 1);
    unsigned int flags = luaL_checkinteger(L, 2);

    assert(dbus_watch_handle(io->watch, flags));
    while (dbus_connection_dispatch(io->conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;

    return 0;
}

static luaL_Reg watch_funcs[] = {
    {"fd", watch_fd},
    {"flags", watch_flags},
    {"enabled", watch_enabled},
    {"handle", watch_handle},
    {NULL, NULL},
};

static int timeout_interval(lua_State *L)
{
    struct ev_timer_wrap *timer = lua_touserdata(L, 1);
    lua_pushnumber(L, dbus_timeout_get_interval(timer->timeout) * 0.001);
    return 1;
}

static int timeout_enabled(lua_State *L)
{
    struct ev_timer_wrap *timer = lua_touserdata(L, 1);
    lua_pushboolean(L, dbus_timeout_get_enabled(timer->timeout));
    return 1;
}

static int timeout_handle(lua_State *L)
{
    struct ev_timer_wrap *timer = lua_touserdata(L, 1);

    assert(dbus_timeout_handle(timer->timeout));
    while (dbus_connection_dispatch(timer->conn) == DBUS_DISPATCH_DATA_REMAINS)
        ;

    return 0;
}

static luaL_Reg timeout_funcs[] = {
    {"interval", timeout_interval},
    {"enabled", timeout_enabled},
    {"handle", timeout_handle},
    {NULL, NULL},
};

static struct ev_io_wrap *ev_io_wrap_add(struct easydbus_state *state)
{
    lua_State *L = state->L;
    struct ev_io_wrap *last;
    struct ev_io_wrap *io = lua_newuserdata(L, sizeof(*io));

    last = state->ios->prev;
    last->next = io;
    state->ios->prev = io;
    io->next = state->ios;
    io->prev = last;

    lua_pushlightuserdata(L, WATCH_MT);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
    io->ref = luaL_ref(L, LUA_REGISTRYINDEX);

    return io;
}

static void ev_io_wrap_remove(struct easydbus_state *state, struct ev_io_wrap *io)
{
    io->next->prev = io->prev;
    io->prev->next = io->next;

    luaL_unref(state->L, LUA_REGISTRYINDEX, io->ref);
}

static struct ev_timer_wrap *ev_timer_wrap_add(struct easydbus_state *state)
{
    lua_State *L = state->L;
    struct ev_timer_wrap *last;
    struct ev_timer_wrap *timer = lua_newuserdata(L, sizeof(*timer));

    last = state->timers->prev;
    last->next = timer;
    state->timers->prev = timer;
    timer->next = state->timers;
    timer->prev = last;

    lua_pushlightuserdata(L, TIMEOUT_MT);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
    timer->ref = luaL_ref(L, LUA_REGISTRYINDEX);

    return timer;
}

static void ev_timer_wrap_remove(struct easydbus_state *state, struct ev_timer_wrap *timer)
{
    timer->next->prev = timer->prev;
    timer->prev->next = timer->next;

    luaL_unref(state->L, LUA_REGISTRYINDEX, timer->ref);
}

void easydbus_enable_ios(struct ev_loop *loop, struct easydbus_state *state)
{
    struct ev_io_wrap *ios = state->ios;
    struct ev_io_wrap *io;
    struct ev_timer_wrap *timers = state->timers;
    struct ev_timer_wrap *timer;
    int interval;

    for (io = ios->next; io != ios; io = io->next) {
        if (dbus_watch_get_enabled(io->watch))
            ev_io_start(loop, &io->io);
    }

    for (timer = timers->next; timer != timers; timer = timer->next) {
        if (dbus_timeout_get_enabled(timer->timeout)) {
            interval = dbus_timeout_get_interval(timer->timeout);
            ev_timer_set(&timer->timer, interval * 0.001, interval * 0.001);
            ev_timer_start(loop, &timer->timer);
        }
    }
}

void easydbus_disable_ios(struct ev_loop *loop, struct easydbus_state *state)
{
    struct ev_io_wrap *ios = state->ios;
    struct ev_io_wrap *io;
    struct ev_timer_wrap *timers = state->timers;
    struct ev_timer_wrap *timer;

    for (io = ios->next; io != ios; io = io->next)
        ev_io_stop(loop, &io->io);

    for (timer = timers->next; timer != timers; timer = timer->next)
        ev_timer_stop(loop, &timer->timer);
}

void easydbus_enable_external_watches(lua_State *L, struct easydbus_state *state)
{
    struct easydbus_external_mainloop *ext = &state->external_mainloop;
    struct ev_io_wrap *ios = state->ios;
    struct ev_io_wrap *io;
    struct ev_timer_wrap *timers = state->timers;
    struct ev_timer_wrap *timer;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ext->watch_add);
    for (io = ios->next; io != ios; io = io->next) {
        lua_pushvalue(L, -1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, io->ref);
        lua_call(L, 1, 0);
    }
    lua_pop(L, 1);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ext->timeout_add);
    for (timer = timers->next; timer != timers; timer = timer->next) {
        lua_pushvalue(L, -1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, timer->ref);
        lua_call(L, 1, 0);
    }
    lua_pop(L, 1);
}

void easydbus_disable_external_watches(lua_State *L, struct easydbus_state *state)
{
    struct easydbus_external_mainloop *ext = &state->external_mainloop;
    struct ev_io_wrap *ios = state->ios;
    struct ev_io_wrap *io;
    struct ev_timer_wrap *timers = state->timers;
    struct ev_timer_wrap *timer;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ext->watch_remove);
    for (io = ios->next; io != ios; io = io->next) {
        lua_pushvalue(L, -1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, io->ref);
        lua_call(L, 1, 0);
    }
    lua_pop(L, 1);

    lua_rawgeti(L, LUA_REGISTRYINDEX, ext->timeout_remove);
    for (timer = timers->next; timer != timers; timer = timer->next) {
        lua_pushvalue(L, -1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, timer->ref);
        lua_call(L, 1, 0);
    }
    lua_pop(L, 1);
}

static dbus_bool_t watch_add(DBusWatch *watch, void *data)
{
    struct ev_loop_wrap *loop_wrap = data;
    struct ev_loop *loop = loop_wrap->loop;
    struct easydbus_state *state = loop_wrap->state;
    struct easydbus_external_mainloop *ext = &state->external_mainloop;
    DBusConnection *conn = loop_wrap->conn;
    unsigned int flags;
    struct ev_io_wrap *io_wrap = ev_io_wrap_add(state);
    struct ev_io *io;

    io = &io_wrap->io;
    io_wrap->watch = watch;
    io_wrap->conn = conn;
    flags = dbus_watch_get_flags(watch);

    g_debug("%s: %p %p %d %u", __FUNCTION__, (void *) watch, (void *) io, dbus_watch_get_unix_fd(watch), flags);

    ev_io_init(io, io_cb, dbus_watch_get_unix_fd(watch),
               flags_dbus_to_ev(flags));

    dbus_watch_set_data(watch, io_wrap, NULL);

    if (state->in_mainloop && dbus_watch_get_enabled(watch))
        ev_io_start(loop, io);

    if (ext->active) {
        lua_State *L = state->L;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ext->watch_add);
        lua_rawgeti(L, LUA_REGISTRYINDEX, io_wrap->ref);
        lua_call(L, 1, 0);
    }

    return TRUE;
}

static void watch_remove(DBusWatch *watch, void *data)
{
    struct ev_loop_wrap *loop_wrap = data;
    struct easydbus_state *state = loop_wrap->state;
    struct easydbus_external_mainloop *ext = &state->external_mainloop;
    struct ev_loop *loop = loop_wrap->loop;
    struct ev_io_wrap *io_wrap = dbus_watch_get_data(watch);
    struct ev_io *io = &io_wrap->io;

    g_debug("%s: %p\n", __FUNCTION__, (void *) io);

    if (state->in_mainloop)
        ev_io_stop(loop, io);

    if (ext->active) {
        lua_State *L = state->L;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ext->watch_remove);
        lua_rawgeti(L, LUA_REGISTRYINDEX, io_wrap->ref);
        lua_call(L, 1, 0);
    }

    ev_io_wrap_remove(state, io_wrap);
}

static void watch_toggle(DBusWatch *watch, void *data)
{
    struct ev_loop_wrap *loop_wrap = data;
    struct easydbus_state *state = loop_wrap->state;
    struct easydbus_external_mainloop *ext = &state->external_mainloop;
    struct ev_loop *loop = loop_wrap->loop;
    struct ev_io_wrap *io_wrap = dbus_watch_get_data(watch);
    struct ev_io *io = &io_wrap->io;

    g_debug("%s: %p\n", __FUNCTION__, (void *) io);

    if (ext->active) {
        lua_State *L = state->L;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ext->watch_toggle);
        lua_rawgeti(L, LUA_REGISTRYINDEX, io_wrap->ref);
        lua_call(L, 1, 0);
    }

    if (!state->in_mainloop)
        return;

    if (dbus_watch_get_enabled(watch))
        ev_io_start(loop, io);
    else
        ev_io_stop(loop, io);
}

static dbus_bool_t timeout_add(DBusTimeout *timeout, void *data)
{
    struct ev_loop_wrap *loop_wrap = data;
    struct easydbus_state *state = loop_wrap->state;
    struct easydbus_external_mainloop *ext = &state->external_mainloop;
    DBusConnection *conn = loop_wrap->conn;
    struct ev_loop *loop = loop_wrap->loop;
    struct ev_timer_wrap *timer_wrap = ev_timer_wrap_add(state);
    struct ev_timer *timer;
    int interval = dbus_timeout_get_interval(timeout);

    timer = &timer_wrap->timer;
    timer_wrap->timeout = timeout;
    timer_wrap->conn = conn;

    g_debug("%s: %p %d", __FUNCTION__, (void *) timeout, interval);

    ev_timer_init(timer, timer_cb, interval * 0.001, interval * 0.001);

    dbus_timeout_set_data(timeout, timer_wrap, NULL);

    if (state->in_mainloop && dbus_timeout_get_enabled(timeout))
        ev_timer_start(loop, timer);

    if (ext->active) {
        lua_State *L = state->L;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ext->timeout_add);
        lua_rawgeti(L, LUA_REGISTRYINDEX, timer_wrap->ref);
        lua_call(L, 1, 0);
    }

    return TRUE;
}

static void timeout_remove(DBusTimeout *timeout, void *data)
{
    struct ev_loop_wrap *loop_wrap = data;
    struct easydbus_state *state = loop_wrap->state;
    struct easydbus_external_mainloop *ext = &state->external_mainloop;
    struct ev_loop *loop = loop_wrap->loop;
    struct ev_timer_wrap *timer_wrap = dbus_timeout_get_data(timeout);
    struct ev_timer *timer = &timer_wrap->timer;

    g_debug("%s: %p\n", __FUNCTION__, (void *) timeout);

    if (state->in_mainloop)
        ev_timer_stop(loop, timer);

    if (ext->active) {
        lua_State *L = state->L;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ext->timeout_remove);
        lua_rawgeti(L, LUA_REGISTRYINDEX, timer_wrap->ref);
        lua_call(L, 1, 0);
    }

    ev_timer_wrap_remove(state, timer_wrap);
}

static void timeout_toggle(DBusTimeout *timeout, void *data)
{
    struct ev_loop_wrap *loop_wrap = data;
    struct easydbus_state *state = loop_wrap->state;
    struct easydbus_external_mainloop *ext = &state->external_mainloop;
    struct ev_loop *loop = loop_wrap->loop;
    struct ev_timer_wrap *timer_wrap = dbus_timeout_get_data(timeout);
    struct ev_timer *timer = &timer_wrap->timer;

    g_debug("%s: %p\n", __FUNCTION__, (void *) timer);

    if (ext->active) {
        lua_State *L = state->L;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ext->watch_toggle);
        lua_rawgeti(L, LUA_REGISTRYINDEX, timer_wrap->ref);
        lua_call(L, 1, 0);
    }

    if (!state->in_mainloop)
        return;

    if (dbus_timeout_get_enabled(timeout))
        ev_timer_start(loop, timer);
    else
        ev_timer_stop(loop, timer);
}

int new_conn(lua_State *L, DBusBusType bus_type)
{
    struct easydbus_state *state = lua_touserdata(L, lua_upvalueindex(1));
    struct ev_loop *loop = state->loop;
    DBusConnection *conn = dbus_bus_get(bus_type, NULL);
    struct ev_loop_wrap *loop_wrap;
    assert(conn);

    /* Check if there is already bus registered */
    lua_pushlightuserdata(L, conn);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isnil(L, -1)) {
        g_debug("There is already a connection");
        dbus_connection_unref(conn);
        return 1;
    }

    loop_wrap = malloc(sizeof(*loop_wrap));
    assert(loop_wrap);
    loop_wrap->loop = loop;
    loop_wrap->conn = conn;
    loop_wrap->state = state;

    dbus_connection_register_fallback(conn, "/", &interface_vtable, state);
    dbus_connection_add_filter(conn, signal_callback, state, NULL);
    dbus_connection_add_filter(conn, standard_methods_callback, state, NULL);

    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    dbus_connection_set_watch_functions(conn, watch_add, watch_remove,
                                        watch_toggle, loop_wrap, free);
    dbus_connection_set_timeout_functions(conn, timeout_add, timeout_remove,
                                          timeout_toggle, loop_wrap, NULL);

    /* Create table with conn userdata, method and signal handlers */
    lua_createtable(L, 3, 0);
    lua_pushlightuserdata(L, conn);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);

    /* Push conn userdata */
    lua_pushlightuserdata(L, conn);
    lua_rawseti(L, -2, 1);

    /* Push method handlers */
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "handlers");
    lua_rawseti(L, -2, 2);

    /* Push signals handlers */
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "signals");
    lua_rawseti(L, -2, 3);

    /* Set metatable */
    lua_pushlightuserdata(L, BUS_MT);
    lua_rawget(L, LUA_REGISTRYINDEX);

    lua_setmetatable(L, -2);

    g_debug("Created conn=%p", (void *) conn);

    return 1;
}

int luaopen_easydbus_bus(lua_State *L)
{
    /* Setup watch mt and push to registry */
    lua_pushlightuserdata(L, WATCH_MT);
    luaL_newlib(L, watch_funcs);
    lua_pushliteral(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);
    lua_rawset(L, LUA_REGISTRYINDEX);

    /* Setup timeout mt and push to registry */
    lua_pushlightuserdata(L, TIMEOUT_MT);
    luaL_newlib(L, timeout_funcs);
    lua_pushliteral(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);
    lua_rawset(L, LUA_REGISTRYINDEX);

    /* Set bus mt */
    luaL_newlibtable(L, bus_funcs);
    lua_pushvalue(L, 1);
    luaL_setfuncs(L, bus_funcs, 1);
    lua_pushliteral(L, "__index");
    lua_pushvalue(L, -2);
    lua_rawset(L, -3);

    /* Set bus mt in registry */
    lua_pushlightuserdata(L, BUS_MT);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);

    return 1;
}
