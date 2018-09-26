// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dlfcn.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>

#include <zircon/dlfcn.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/log.h>

#include <fuchsia/io/c/fidl.h>
#include <lib/fdio/util.h>
#include <lib/fdio/remoteio.h>
#include <lib/fidl/coding.h>

#include "devcoordinator.h"
#include "devhost.h"
#include "devhost-main.h"
#include "log.h"

uint32_t log_flags = LOG_ERROR | LOG_INFO;

struct proxy_iostate {
    zx_device_t* dev;
    port_handler_t ph;
};
static void proxy_ios_create(zx_device_t* dev, zx_handle_t h);
static void proxy_ios_destroy(zx_device_t* dev);

#define proxy_ios_from_ph(ph) containerof(ph, proxy_iostate_t, ph)

#define ios_from_ph(ph) containerof(ph, devhost_iostate_t, ph)

static zx_status_t dh_handle_dc_rpc(port_handler_t* ph, zx_signals_t signals, uint32_t evt);

static port_t dh_port;

typedef struct devhost_iostate iostate_t;

static iostate_t root_ios = []() {
    iostate_t ios;
    ios.ph.waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
    ios.ph.func = dh_handle_dc_rpc;
    return ios;
}();

static fbl::DoublyLinkedList<zx_driver*> dh_drivers;

static const char* mkdevpath(zx_device_t* dev, char* path, size_t max) {
    if (dev == nullptr) {
        return "";
    }
    if (max < 1) {
        return "<invalid>";
    }
    char* end = path + max;
    char sep = 0;

    while (dev) {
        *(--end) = sep;

        size_t len = strlen(dev->name);
        if (len > (size_t)(end - path)) {
            break;
        }
        end -= len;
        memcpy(end, dev->name, len);
        sep = '/';
        dev = dev->parent;
    }
    return end;
}

static uint32_t logflagval(char* flag) {
    if (!strcmp(flag, "error")) {
        return DDK_LOG_ERROR;
    }
    if (!strcmp(flag, "warn")) {
        return DDK_LOG_WARN;
    }
    if (!strcmp(flag, "info")) {
        return DDK_LOG_INFO;
    }
    if (!strcmp(flag, "trace")) {
        return DDK_LOG_TRACE;
    }
    if (!strcmp(flag, "spew")) {
        return DDK_LOG_SPEW;
    }
    if (!strcmp(flag, "debug1")) {
        return DDK_LOG_DEBUG1;
    }
    if (!strcmp(flag, "debug2")) {
        return DDK_LOG_DEBUG2;
    }
    if (!strcmp(flag, "debug3")) {
        return DDK_LOG_DEBUG3;
    }
    if (!strcmp(flag, "debug4")) {
        return DDK_LOG_DEBUG4;
    }
    return static_cast<uint32_t>(strtoul(flag, nullptr, 0));
}

static void logflag(char* flag, uint32_t* flags) {
    if (*flag == '+') {
        *flags |= logflagval(flag + 1);
    } else if (*flag == '-') {
        *flags &= ~logflagval(flag + 1);
    }
}

static zx_status_t dh_find_driver(const char* libname, zx_handle_t vmo, zx_driver_t** out) {
    // check for already-loaded driver first
    for (auto& drv : dh_drivers) {
        if (!strcmp(libname, drv.libname)) {
            *out = &drv;
            zx_handle_close(vmo);
            return drv.status;
        }
    }

    size_t len = strlen(libname) + 1;
    auto drv = static_cast<zx_driver_t*>(calloc(1, sizeof(zx_driver_t) + len));
    if (drv == nullptr) {
        zx_handle_close(vmo);
        return ZX_ERR_NO_MEMORY;
    }
    memcpy((void*) (drv + 1), libname, len);
    drv->libname = (const char*) (drv + 1);
    dh_drivers.push_back(drv);
    *out = drv;

    void* dl = dlopen_vmo(vmo, RTLD_NOW);
    if (dl == nullptr) {
        log(ERROR, "devhost: cannot load '%s': %s\n", libname, dlerror());
        drv->status = ZX_ERR_IO;
        goto done;
    }

    const zircon_driver_note_t* dn;
    dn = static_cast<const zircon_driver_note_t*>(dlsym(dl, "__zircon_driver_note__"));
    if (dn == nullptr) {
        log(ERROR, "devhost: driver '%s' missing __zircon_driver_note__ symbol\n", libname);
        drv->status = ZX_ERR_IO;
        goto done;
    }
    zx_driver_rec_t* dr;
    dr = static_cast<zx_driver_rec_t*>(dlsym(dl, "__zircon_driver_rec__"));
    if (dr == nullptr) {
        log(ERROR, "devhost: driver '%s' missing __zircon_driver_rec__ symbol\n", libname);
        drv->status = ZX_ERR_IO;
        goto done;
    }
    if (!dr->ops) {
        log(ERROR, "devhost: driver '%s' has nullptr ops\n", libname);
        drv->status = ZX_ERR_INVALID_ARGS;
        goto done;
    }
    if (dr->ops->version != DRIVER_OPS_VERSION) {
        log(ERROR, "devhost: driver '%s' has bad driver ops version %" PRIx64
            ", expecting %" PRIx64 "\n", libname,
            dr->ops->version, DRIVER_OPS_VERSION);
        drv->status = ZX_ERR_INVALID_ARGS;
        goto done;
    }

    drv->driver_rec = dr;
    drv->name = dn->payload.name;
    drv->ops = dr->ops;
    dr->driver = drv;

    // check for dprintf log level flags
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "driver.%s.log", drv->name);
    char* log;
    log = getenv(tmp);
    if (log) {
        while (log) {
            char* sep = strchr(log, ',');
            if (sep) {
                *sep = 0;
                logflag(log, &dr->log_flags);
                *sep = ',';
                log = sep + 1;
            } else {
                logflag(log, &dr->log_flags);
                break;
            }
        }
        log(INFO, "devhost: driver '%s': log flags set to: 0x%x\n", drv->name, dr->log_flags);
    }

    if (drv->ops->init) {
        drv->status = drv->ops->init(&drv->ctx);
        if (drv->status < 0) {
            log(ERROR, "devhost: driver '%s' failed in init: %d\n",
                libname, drv->status);
        }
    } else {
        drv->status = ZX_OK;
    }

done:
    zx_handle_close(vmo);
    return drv->status;
}

static void dh_send_status(zx_handle_t h, zx_status_t status) {
    dc_msg_t reply = {};
    reply.txid = 0;
    reply.op = DC_OP_STATUS;
    reply.status = status;
    zx_channel_write(h, 0, &reply, sizeof(reply), nullptr, 0);
}

static zx_status_t dh_null_reply(fidl_txn_t* reply, const fidl_msg_t* msg) {
    return ZX_OK;
}

static fidl_txn_t dh_null_txn = {
    .reply = dh_null_reply,
};

static zx_status_t dh_handle_rpc_read(zx_handle_t h, iostate_t* ios) {
    dc_msg_t msg;
    zx_handle_t hin[3];
    uint32_t msize = sizeof(msg);
    uint32_t hcount = 3;

    zx_status_t r;
    if ((r = zx_channel_read(h, 0, &msg, hin, msize,
                             hcount, &msize, &hcount)) < 0) {
        return r;
    }

    char buffer[512];
    const char* path = mkdevpath(ios->dev, buffer, sizeof(buffer));

    if (msize >= sizeof(fidl_message_header_t) && msg.op == fuchsia_io_DirectoryOpenOrdinal) {
        log(RPC_RIO, "devhost[%s] FIDL OPEN\n", path);

        fidl_msg_t fidl_msg = {
            .bytes = &msg,
            .handles = hin,
            .num_bytes = msize,
            .num_handles = hcount,
        };

        if ((r = devhost_fidl_handler(&fidl_msg, &dh_null_txn, ios)) != ZX_OK) {
            log(ERROR, "devhost: OPEN failed: %d\n", r);
            return r;
        }

        return ZX_OK;
    }

    const void* data;
    const char* name;
    const char* args;
    if ((r = dc_msg_unpack(&msg, msize, &data, &name, &args)) < 0) {
        goto fail;
    }
    switch (msg.op) {
    case DC_OP_CREATE_DEVICE_STUB: {
        log(RPC_IN, "devhost[%s] create device stub drv='%s'\n", path, name);
        if (hcount != 1) {
            r = ZX_ERR_INVALID_ARGS;
            goto fail;
        }
        iostate_t* newios = static_cast<iostate_t*>(calloc(1, sizeof(iostate_t)));
        if (newios == nullptr) {
            r = ZX_ERR_NO_MEMORY;
            break;
        }

        //TODO: dev->ops and other lifecycle bits
        // no name means a dummy proxy device
        if ((newios->dev = static_cast<zx_device_t*>(calloc(1, sizeof(zx_device_t)))) == nullptr) {
            free(newios);
            r = ZX_ERR_NO_MEMORY;
            break;
        }
        zx_device_t* dev = newios->dev;
        strcpy(dev->name, "proxy");
        dev->protocol_id = msg.protocol_id;
        dev->ops = &device_default_ops;
        dev->rpc = hin[0];
        dev->refcount = 1;
        list_initialize(&dev->children);

        newios->ph.handle = hin[0];
        newios->ph.waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
        newios->ph.func = dh_handle_dc_rpc;
        if ((r = port_wait(&dh_port, &newios->ph)) < 0) {
            free(newios->dev);
            free(newios);
            break;
        }
        log(RPC_IN, "devhost[%s] created '%s' ios=%p\n", path, name, newios);
        return ZX_OK;
    }

    case DC_OP_CREATE_DEVICE: {
        // This does not operate under the devhost api lock,
        // since the newly created device is not visible to
        // any API surface until a driver is bound to it.
        // (which can only happen via another message on this thread)
        log(RPC_IN, "devhost[%s] create device drv='%s' args='%s'\n", path, name, args);

        // hin: rpc, vmo, optional-rsrc
        if (hcount == 2) {
            hin[2] = ZX_HANDLE_INVALID;
        } else if (hcount != 3) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        iostate_t* newios = static_cast<iostate_t*>(calloc(1, sizeof(iostate_t)));
        if (newios == nullptr) {
            r = ZX_ERR_NO_MEMORY;
            break;
        }

        // named driver -- ask it to create the device
        zx_driver_t* drv;
        if ((r = dh_find_driver(name, hin[1], &drv)) < 0) {
            free(newios);
            log(ERROR, "devhost[%s] driver load failed: %d\n", path, r);
            break;
        }
        if (drv->ops->create) {
            // magic cookie for device create handshake
            zx_device_t parent = {};
            char dummy_name[sizeof(parent.name)] = "device_create dummy";
            memcpy(&parent.name, &dummy_name, sizeof(parent.name));

            creation_context_t ctx = {
                .parent = &parent,
                .child = nullptr,
                .rpc = hin[0],
            };
            devhost_set_creation_context(&ctx);
            r = drv->ops->create(drv->ctx, &parent, "proxy", args, hin[2]);
            devhost_set_creation_context(nullptr);

            if (r < 0) {
                log(ERROR, "devhost[%s] driver create() failed: %d\n", path, r);
                break;
            }
            if ((newios->dev = ctx.child) == nullptr) {
                log(ERROR, "devhost[%s] driver create() failed to create a device!", path);
                r = ZX_ERR_BAD_STATE;
                break;
            }
        } else {
            log(ERROR, "devhost[%s] driver create() not supported\n", path);
            r = ZX_ERR_NOT_SUPPORTED;
            break;
        }
        //TODO: inform devcoord

        newios->ph.handle = hin[0];
        newios->ph.waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
        newios->ph.func = dh_handle_dc_rpc;
        if ((r = port_wait(&dh_port, &newios->ph)) < 0) {
            free(newios);
            break;
        }
        log(RPC_IN, "devhost[%s] created '%s' ios=%p\n", path, name, newios);
        return ZX_OK;
    }

    case DC_OP_BIND_DRIVER:
        if (hcount != 1) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        //TODO: api lock integration
        log(RPC_IN, "devhost[%s] bind driver '%s'\n", path, name);
        zx_driver_t* drv;
        if (ios->dev->flags & DEV_FLAG_DEAD) {
            log(ERROR, "devhost[%s] bind to removed device disallowed\n", path);
            r = ZX_ERR_IO_NOT_PRESENT;
        } else if ((r = dh_find_driver(name, hin[0], &drv)) < 0) {
            log(ERROR, "devhost[%s] driver load failed: %d\n", path, r);
        } else {
            if (drv->ops->bind) {
                creation_context_t ctx = {
                    .parent = ios->dev,
                    .child = nullptr,
                    .rpc = ZX_HANDLE_INVALID,
                };
                devhost_set_creation_context(&ctx);
                r = drv->ops->bind(drv->ctx, ios->dev);
                devhost_set_creation_context(nullptr);

                if ((r == ZX_OK) && (ctx.child == nullptr)) {
                    printf("devhost: WARNING: driver '%s' did not add device in bind()\n", name);
                }
                if (r < 0) {
                    log(ERROR, "devhost[%s] bind driver '%s' failed: %d\n", path, name, r);
                }
            } else {
                if (!drv->ops->create) {
                    log(ERROR, "devhost[%s] neither create nor bind are implemented: '%s'\n",
                        path, name);
                }
                r = ZX_ERR_NOT_SUPPORTED;
            }
        }
        dh_send_status(h, r);
        return ZX_OK;

    case DC_OP_CONNECT_PROXY:
        if (hcount != 1) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        log(RPC_SDW, "devhost[%s] connect proxy rpc\n", path);
        ios->dev->ops->rxrpc(ios->dev->ctx, ZX_HANDLE_INVALID);
        proxy_ios_create(ios->dev, hin[0]);
        return ZX_OK;

    case DC_OP_SUSPEND: {
        if (hcount != 0) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        // call suspend on the device this devhost is rooted on
        zx_device_t* device = ios->dev;
        while (device->parent != nullptr) {
            device = device->parent;
        }
        DM_LOCK();
        r = devhost_device_suspend(device, msg.value);
        DM_UNLOCK();
        dh_send_status(h, r);
        return ZX_OK;
    }

    case DC_OP_REMOVE_DEVICE:
        if (hcount != 0) {
            r = ZX_ERR_INVALID_ARGS;
            break;
        }
        device_remove(ios->dev);
        return ZX_OK;

    default:
        log(ERROR, "devhost[%s] invalid rpc op %08x\n", path, msg.op);
        r = ZX_ERR_NOT_SUPPORTED;
    }

fail:
    while (hcount > 0) {
        zx_handle_close(hin[--hcount]);
    }
    return r;
}

// handles devcoordinator rpc
static zx_status_t dh_handle_dc_rpc(port_handler_t* ph, zx_signals_t signals, uint32_t evt) {
    iostate_t* ios = ios_from_ph(ph);

    if (evt != 0) {
        // we send an event to request the destruction
        // of an iostate, to ensure that's the *last*
        // packet about the iostate that we get
        free(ios);
        return ZX_ERR_STOP;
    }
    if (ios->dead) {
        // ports does not let us cancel packets that are
        // already in the queue, so the dead flag enables us
        // to ignore them
        return ZX_ERR_STOP;
    }
    if (signals & ZX_CHANNEL_READABLE) {
        zx_status_t r = dh_handle_rpc_read(ph->handle, ios);
        if (r != ZX_OK) {
            log(ERROR, "devhost: devmgr rpc unhandleable ios=%p r=%d. fatal.\n", ios, r);
            exit(0);
        }
        return r;
    }
    if (signals & ZX_CHANNEL_PEER_CLOSED) {
        log(ERROR, "devhost: devmgr disconnected! fatal. (ios=%p)\n", ios);
        exit(0);
    }
    log(ERROR, "devhost: no work? %08x\n", signals);
    return ZX_OK;
}

// handles remoteio rpc
static zx_status_t dh_handle_fidl_rpc(port_handler_t* ph, zx_signals_t signals, uint32_t evt) {
    iostate_t* ios = ios_from_ph(ph);

    zx_status_t r;
    if (signals & ZX_CHANNEL_READABLE) {
        if ((r = zxfidl_handler(ph->handle, devhost_fidl_handler, ios)) == ZX_OK) {
            return ZX_OK;
        }
    } else if (signals & ZX_CHANNEL_PEER_CLOSED) {
        zxfidl_handler(ZX_HANDLE_INVALID, devhost_fidl_handler, ios);
        r = ZX_ERR_STOP;
    } else {
        printf("dh_handle_fidl_rpc: invalid signals %x\n", signals);
        exit(0);
    }

    // We arrive here if handle_rpc was a clean close (ERR_DISPATCHER_DONE),
    // or close-due-to-error (non-ZX_OK), or if the channel was closed
    // out from under us (ZX_ERR_STOP).  In all cases, the ios's reference to
    // the device was released, and will no longer be used, so we will free
    // it before returning.
    zx_handle_close(ios->ph.handle);
    free(ios);
    return r;
}


// Handling RPC From Proxy Devices to BusDevs

static zx_status_t dh_handle_proxy_rpc(port_handler_t* ph, zx_signals_t signals, uint32_t evt) {
    proxy_iostate_t* ios = proxy_ios_from_ph(ph);

    if (evt != 0) {
        log(RPC_SDW, "proxy-rpc: destroy (ios=%p)\n", ios);
        // we send an event to request the destruction
        // of an iostate, to ensure that's the *last*
        // packet about the iostate that we get
        free(ios);
        return ZX_ERR_STOP;
    }
    if (ios->dev == nullptr) {
        log(RPC_SDW, "proxy-rpc: stale rpc? (ios=%p)\n", ios);
        // ports does not let us cancel packets that are
        // already in the queue, so the dead flag enables us
        // to ignore them
        return ZX_ERR_STOP;
    }
    if (signals & ZX_CHANNEL_READABLE) {
        log(RPC_SDW, "proxy-rpc: rpc readable (ios=%p,dev=%p)\n", ios, ios->dev);
        zx_status_t r;
        r = ios->dev->ops->rxrpc(ios->dev->ctx, ph->handle);
        if (r != ZX_OK) {
            log(RPC_SDW, "proxy-rpc: rpc cb error %d (ios=%p,dev=%p)\n", r, ios, ios->dev);
destroy:
            ios->dev->proxy_ios = nullptr;
            zx_handle_close(ios->ph.handle);
            free(ios);
            return ZX_ERR_STOP;
        }
        return ZX_OK;
    }
    if (signals & ZX_CHANNEL_PEER_CLOSED) {
        log(RPC_SDW, "proxy-rpc: peer closed (ios=%p,dev=%p)\n", ios, ios->dev);
        goto destroy;
    }
    log(ERROR, "devhost: no work? %08x\n", signals);
    return ZX_OK;
}

static void proxy_ios_create(zx_device_t* dev, zx_handle_t h) {
    if (dev->proxy_ios) {
        proxy_ios_destroy(dev);
    }

    proxy_iostate_t* ios;
    if ((ios = static_cast<proxy_iostate_t*>(calloc(sizeof(proxy_iostate_t), 1))) == nullptr) {
        zx_handle_close(h);
        return;
    }

    ios->dev = dev;
    ios->ph.handle = h;
    ios->ph.waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
    ios->ph.func = dh_handle_proxy_rpc;
    if (port_wait(&dh_port, &ios->ph) != ZX_OK) {
        zx_handle_close(h);
        free(ios);
    } else {
        dev->proxy_ios = ios;
    }
}

static void proxy_ios_destroy(zx_device_t* dev) {
    proxy_iostate_t* ios = dev->proxy_ios;
    if (ios) {
        dev->proxy_ios = nullptr;

        // mark iostate detached
        ios->dev = nullptr;

        // cancel any pending waits
        port_cancel(&dh_port, &ios->ph);

        zx_handle_close(ios->ph.handle);
        ios->ph.handle = ZX_HANDLE_INVALID;

        // queue an event to destroy the iostate
        port_queue(&dh_port, &ios->ph, 1);
    }
}


#define LOGBUF_MAX (ZX_LOG_RECORD_MAX - sizeof(zx_log_record_t))

static zx_handle_t devhost_log_handle;

static ssize_t _devhost_log_write(uint32_t flags, const void* _data, size_t len) {
    static thread_local struct {
        uint32_t next;
        zx_handle_t handle;
        char data[LOGBUF_MAX];
    }* ctx = nullptr;

    if (ctx == nullptr) {
        if ((ctx = static_cast<decltype(ctx)>(calloc(1, sizeof(*ctx)))) == nullptr) {
            return len;
        }
        ctx->handle = devhost_log_handle;
    }

    const char* data = static_cast<const char*>(_data);
    size_t r = len;

    while (len-- > 0) {
        char c = *data++;
        if (c == '\n') {
            if (ctx->next) {
flush_ctx:
                zx_debuglog_write(ctx->handle, flags, ctx->data, ctx->next);
                ctx->next = 0;
            }
            continue;
        }
        if (c < ' ') {
            continue;
        }
        ctx->data[ctx->next++] = c;
        if (ctx->next == LOGBUF_MAX) {
            goto flush_ctx;
        }
    }
    return r;
}

__EXPORT void driver_printf(uint32_t flags, const char* fmt, ...) {
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    if (r > (int)sizeof(buffer)) {
        r = sizeof(buffer);
    }

    _devhost_log_write(flags, buffer, r);
}

static ssize_t devhost_log_write(void* cookie, const void* data, size_t len) {
    return _devhost_log_write(0, data, len);
}

static void devhost_io_init(void) {
    if (zx_debuglog_create(ZX_HANDLE_INVALID, 0, &devhost_log_handle) < 0) {
        return;
    }
    fdio_t* io;
    if ((io = fdio_output_create(devhost_log_write, nullptr)) == nullptr) {
        return;
    }
    close(1);
    fdio_bind_to_fd(io, 1, 0);
    dup2(1, 2);
}

// Send message to devcoordinator asking to add child device to
// parent device.  Called under devhost api lock.
zx_status_t devhost_add(zx_device_t* parent, zx_device_t* child, const char* proxy_args,
                        const zx_device_prop_t* props, uint32_t prop_count) {
    char buffer[512];
    const char* path = mkdevpath(parent, buffer, sizeof(buffer));
    log(RPC_OUT, "devhost[%s] add '%s'\n", path, child->name);

    const char* libname = child->driver->libname;
    size_t namelen = strlen(libname) + strlen(child->name) + 2;
    char name[namelen];
    snprintf(name, namelen, "%s,%s", libname, child->name);

    zx_status_t r;
    iostate_t* ios = static_cast<iostate_t*>(calloc(1, sizeof(*ios)));
    if (ios == nullptr) {
        r = ZX_ERR_NO_MEMORY;
        goto fail;
    }

    dc_msg_t msg;
    uint32_t msglen;
    if ((r = dc_msg_pack(&msg, &msglen,
                         props, prop_count * sizeof(zx_device_prop_t),
                         name, proxy_args)) < 0) {
        goto fail;
    }
    msg.op = (child->flags & DEV_FLAG_INVISIBLE) ? DC_OP_ADD_DEVICE_INVISIBLE : DC_OP_ADD_DEVICE;
    msg.protocol_id = child->protocol_id;

    // handles: remote endpoint, resource (optional)
    zx_handle_t hrpc, hsend;
    if ((r = zx_channel_create(0, &hrpc, &hsend)) < 0) {
        goto fail;
    }

    dc_status_t rsp;
    if ((r = dc_msg_rpc(parent->rpc, &msg, msglen, &hsend, 1, &rsp, sizeof(rsp), nullptr, nullptr)) < 0) {
        log(ERROR, "devhost[%s] add '%s': rpc failed: %d\n", path, child->name, r);
    } else {
        ios->dev = child;
        ios->ph.handle = hrpc;
        ios->ph.waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
        ios->ph.func = dh_handle_dc_rpc;
        if ((r = port_wait(&dh_port, &ios->ph)) == ZX_OK) {
            child->rpc = hrpc;
            child->ios = ios;
            return ZX_OK;
        }

    }
    zx_handle_close(hrpc);
    free(ios);
    return r;

fail:
    free(ios);
    return r;
}

static zx_status_t devhost_rpc_etc(zx_device_t* dev, uint32_t op,
                                   const char* args, const char* opname,
                                   uint32_t value, const void* data, size_t datalen,
                                   dc_status_t* rsp, size_t rsp_len, size_t* actual,
                                   zx_handle_t* outhandle) {
    char buffer[512];
    const char* path = mkdevpath(dev, buffer, sizeof(buffer));
    log(RPC_OUT, "devhost[%s] %s args='%s'\n", path, opname, args ? args : "");
    dc_msg_t msg;
    uint32_t msglen;
    zx_status_t r;
    if ((r = dc_msg_pack(&msg, &msglen, data, datalen, nullptr, args)) < 0) {
        return r;
    }
    msg.op = op;
    msg.value = value;
    if ((r = dc_msg_rpc(dev->rpc, &msg, msglen, nullptr, 0, rsp, rsp_len, actual, outhandle)) < 0) {
        if (!(op == DC_OP_GET_METADATA && r == ZX_ERR_NOT_FOUND)) {
            log(ERROR, "devhost: rpc:%s failed: %d\n", opname, r);
        }
    }
    return r;
}

static zx_status_t devhost_rpc(zx_device_t* dev, uint32_t op,
                               const char* args, const char* opname,
                               dc_status_t* rsp, size_t rsp_len,
                               zx_handle_t* outhandle) {
    return devhost_rpc_etc(dev, op, args, opname, 0, nullptr, 0, rsp, rsp_len, nullptr, outhandle);
}

void devhost_make_visible(zx_device_t* dev) {
    dc_status_t rsp;
    devhost_rpc(dev, DC_OP_MAKE_VISIBLE, nullptr, "make-visible", &rsp, sizeof(rsp), nullptr);
}

// Send message to devcoordinator informing it that this device
// is being removed.  Called under devhost api lock.
zx_status_t devhost_remove(zx_device_t* dev) {
    devhost_iostate_t* ios = static_cast<devhost_iostate_t*>(dev->ios);
    if (ios == nullptr) {
        log(ERROR, "removing device %p, ios is nullptr\n", dev);
        return ZX_ERR_INTERNAL;
    }

    log(DEVLC, "removing device %p, ios %p\n", dev, ios);

    // Make this iostate inactive (stop accepting RPCs for it)
    //
    // If the remove is happening on a different thread than
    // the rpc handler, the handler might observe the peer
    // before devhost_simple_rpc() returns.
    ios->dev = nullptr;
    ios->dead = true;

    // ensure we get no further events
    //TODO: this does not work yet, ports limitation
    port_cancel(&dh_port, &ios->ph);
    ios->ph.handle = ZX_HANDLE_INVALID;
    dev->ios = nullptr;

    dc_status_t rsp;
    devhost_rpc(dev, DC_OP_REMOVE_DEVICE, nullptr, "remove-device", &rsp, sizeof(rsp), nullptr);

    // shut down our rpc channel
    zx_handle_close(dev->rpc);
    dev->rpc = ZX_HANDLE_INVALID;

    // queue an event to destroy the iostate
    port_queue(&dh_port, &ios->ph, 1);

    // shut down our proxy rpc channel if it exists
    proxy_ios_destroy(dev);

    return ZX_OK;
}

zx_status_t devhost_get_topo_path(zx_device_t* dev, char* path, size_t max, size_t* actual) {
    zx_device_t* remote_dev = dev;
    if (dev->flags & DEV_FLAG_INSTANCE) {
        // Instances cannot be opened a second time. If dev represents an instance, return the path
        // to its parent, prefixed with an '@'.
        if (max < 1) {
            return ZX_ERR_BUFFER_TOO_SMALL;
        }
        path[0] = '@';
        path++;
        max--;
        remote_dev = dev->parent;
    }

    struct {
        dc_status_t rsp;
        char path[DC_PATH_MAX];
    } reply;
    zx_status_t r;
    if ((r = devhost_rpc(remote_dev, DC_OP_GET_TOPO_PATH, nullptr, "get-topo-path",
                         &reply.rsp, sizeof(reply), nullptr)) < 0) {
        return r;
    }
    reply.path[DC_PATH_MAX - 1] = 0;
    size_t len = strlen(reply.path) + 1;
    if (len > max) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }

    memcpy(path, reply.path, len);
    *actual = len;
    if (dev->flags & DEV_FLAG_INSTANCE) *actual += 1;
    return ZX_OK;
}

zx_status_t devhost_device_bind(zx_device_t* dev, const char* drv_libname) {
    dc_status_t rsp;
    return devhost_rpc(dev, DC_OP_BIND_DEVICE, drv_libname,
                       "bind-device", &rsp, sizeof(rsp), nullptr);
}

zx_status_t devhost_load_firmware(zx_device_t* dev, const char* path,
                                  zx_handle_t* vmo, size_t* size) {
    if ((vmo == nullptr) || (size == nullptr)) {
        return ZX_ERR_INVALID_ARGS;
    }

    struct {
        dc_status_t rsp;
        size_t size;
    } reply;
    zx_status_t r;
    if ((r = devhost_rpc(dev, DC_OP_LOAD_FIRMWARE, path, "load-firmware",
                         &reply.rsp, sizeof(reply), vmo)) < 0) {
        return r;
    }
    if (*vmo == ZX_HANDLE_INVALID) {
        return ZX_ERR_INTERNAL;
    }
    *size = reply.size;
    return ZX_OK;
}

zx_status_t devhost_get_metadata(zx_device_t* dev, uint32_t type, void* buf, size_t buflen,
                                 size_t* actual) {
    if (!buf) {
        return ZX_ERR_INVALID_ARGS;
    }

    struct {
        dc_status_t rsp;
        uint8_t data[DC_MAX_DATA];
    } reply;
    zx_status_t r;
    size_t resp_actual = 0;
    if ((r = devhost_rpc_etc(dev, DC_OP_GET_METADATA, nullptr, "get-metadata", type, nullptr, 0,
                             &reply.rsp, sizeof(reply), &resp_actual, nullptr)) < 0) {
        return r;
    }
    if (resp_actual < sizeof(reply.rsp)) {
        return ZX_ERR_INTERNAL;
    }
    resp_actual -= sizeof(reply.rsp);
    if (resp_actual > buflen) {
        return ZX_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(buf, reply.data, resp_actual);
    if (actual) {
        *actual = resp_actual;
    }

    return ZX_OK;
}

zx_status_t devhost_add_metadata(zx_device_t* dev, uint32_t type, const void* data,
                                 size_t length) {
    dc_status_t rsp;

    if (!data && length) {
        return ZX_ERR_INVALID_ARGS;
    }
    return devhost_rpc_etc(dev, DC_OP_ADD_METADATA, nullptr, "add-metadata", type, data, length,
                            &rsp, sizeof(rsp), nullptr, nullptr);
}

zx_status_t devhost_publish_metadata(zx_device_t* dev, const char* path, uint32_t type,
                                     const void* data, size_t length) {
    dc_status_t rsp;

    if (!path || (!data && length)) {
        return ZX_ERR_INVALID_ARGS;
    }
    return devhost_rpc_etc(dev, DC_OP_PUBLISH_METADATA, path, "publish-metadata", type, data,
                           length, &rsp, sizeof(rsp), nullptr, nullptr);
}

zx_handle_t root_resource_handle;


zx_status_t devhost_start_iostate(devhost_iostate_t* ios, zx_handle_t h) {
    ios->ph.handle = h;
    ios->ph.waitfor = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED;
    ios->ph.func = dh_handle_fidl_rpc;
    return port_wait(&dh_port, &ios->ph);
}

__EXPORT int device_host_main(int argc, char** argv) {
    devhost_io_init();

    log(TRACE, "devhost: main()\n");

    root_ios.ph.handle = zx_take_startup_handle(PA_HND(PA_USER0, 0));
    if (root_ios.ph.handle == ZX_HANDLE_INVALID) {
        log(ERROR, "devhost: rpc handle invalid\n");
        return -1;
    }

    root_resource_handle = zx_take_startup_handle(PA_HND(PA_RESOURCE, 0));
    if (root_resource_handle == ZX_HANDLE_INVALID) {
        log(ERROR, "devhost: no root resource handle!\n");
    }

    zx_status_t r;
    if ((r = port_init(&dh_port)) < 0) {
        log(ERROR, "devhost: could not create port: %d\n", r);
        return -1;
    }
    if ((r = port_wait(&dh_port, &root_ios.ph)) < 0) {
        log(ERROR, "devhost: could not watch rpc channel: %d\n", r);
        return -1;
    }

    r = port_dispatch(&dh_port, ZX_TIME_INFINITE, false);
    log(ERROR, "devhost: port dispatch finished: %d\n", r);

    return 0;
}
