// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bootfs-service.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <utility>

#include <fbl/algorithm.h>
#include <fs/connection.h>
#include <launchpad/launchpad.h>
#include <lib/bootfs/parser.h>
#include <zircon/compiler.h>
#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/status.h>

#include "util.h"

namespace bootsvc {

zx_status_t BootfsService::Create(async_dispatcher_t* dispatcher,
                                  fbl::RefPtr<BootfsService>* out) {
    auto svc = fbl::AdoptRef(new BootfsService());

    zx_status_t status = memfs::CreateFilesystem("<root>", &svc->vfs_, &svc->root_);
    if (status != ZX_OK) {
        return status;
    }

    svc->vfs_.SetDispatcher(dispatcher);
    *out = std::move(svc);
    return ZX_OK;
}

zx_status_t BootfsService::AddBootfs(zx::vmo bootfs_vmo) {
    bootfs::Parser parser;
    zx_status_t status = parser.Init(zx::unowned_vmo(bootfs_vmo));
    if (status != ZX_OK) {
        return status;
    }

    // Load all of the entries in the bootfs into the FS
    status = parser.Parse([this, &bootfs_vmo](const bootfs_entry_t *entry) -> zx_status_t {
        PublishUnownedVmo(entry->name, bootfs_vmo, entry->data_off, entry->data_len);
        return ZX_OK;
    });
    // Add this VMO to our list of parts even on failure, since we may have
    // added a file
    owned_vmos_.push_back(std::move(bootfs_vmo));
    return status;
}

zx_status_t BootfsService::CreateRootConnection(zx::channel* out) {
    zx::channel local, remote;
    zx_status_t status = zx::channel::create(0, &local, &remote);
    if (status != ZX_OK) {
        return status;
    }

    auto conn = fbl::make_unique<fs::Connection>(&vfs_, root_, std::move(local),
                                                 ZX_FS_FLAG_DIRECTORY | ZX_FS_RIGHT_READABLE);
    status = vfs_.ServeConnection(std::move(conn));
    if (status != ZX_OK) {
        return status;
    }
    *out = std::move(remote);
    return ZX_OK;
}

zx_status_t BootfsService::Open(const char* path, zx::vmo* vmo, size_t* size) {
    fbl::RefPtr<fs::Vnode> node;
    fbl::StringPiece path_out;
    zx_status_t status = vfs_.Open(root_, &node, path, &path_out,
                                   ZX_FS_RIGHT_READABLE | ZX_FS_FLAG_NOREMOTE, 0);
    if (status != ZX_OK) {
        return status;
    }
    ZX_ASSERT(path_out.size() == 0);

    fuchsia_io_NodeInfo info;
    status = node->GetHandles(ZX_FS_RIGHT_READABLE, &info);
    if (status != ZX_OK) {
        return status;
    }

    if (info.tag != fuchsia_io_NodeInfoTag_vmofile) {
        return ZX_ERR_WRONG_TYPE;
    }
    ZX_ASSERT(info.vmofile.offset == 0);
    zx::vmo underlying_vmo(info.vmofile.vmo);

    *vmo = std::move(underlying_vmo);
    *size = info.vmofile.length;
    return ZX_OK;
}

BootfsService::~BootfsService() {
    auto callback = [parts(std::move(owned_vmos_))](zx_status_t status) mutable {
        parts.reset();
    };
    vfs_.Shutdown(std::move(callback));
}

zx_status_t BootfsService::PublishVmo(const char* path, zx::vmo vmo, zx_off_t off,
                                      size_t len) {
    zx_status_t status = PublishUnownedVmo(path, vmo, off, len);
    if (status != ZX_OK) {
        return status;
    }
    owned_vmos_.push_back(std::move(vmo));
    return ZX_OK;
}

zx_status_t BootfsService::PublishUnownedVmo(const char* path, const zx::vmo& vmo, zx_off_t off,
                                             size_t len) {
    ZX_ASSERT(root_ != nullptr);
    fbl::RefPtr<memfs::VnodeDir> vnb(root_);
    zx_status_t r;
    if ((path[0] == '/') || (path[0] == 0))
        return ZX_ERR_INVALID_ARGS;
    for (;;) {
        const char* nextpath = strchr(path, '/');
        if (nextpath == nullptr) {
            if (path[0] == 0) {
                return ZX_ERR_INVALID_ARGS;
            }
            return vnb->vfs()->CreateFromVmo(vnb.get(), fbl::StringPiece(path, strlen(path)),
                                             vmo.get(), off, len);
        } else {
            if (nextpath == path) {
                return ZX_ERR_INVALID_ARGS;
            }

            fbl::RefPtr<fs::Vnode> out;
            r = vnb->Lookup(&out, fbl::StringPiece(path, nextpath - path));
            if (r == ZX_ERR_NOT_FOUND) {
                r = vnb->Create(&out, fbl::StringPiece(path, nextpath - path), S_IFDIR);
            }

            if (r < 0) {
                return r;
            }
            vnb = fbl::RefPtr<memfs::VnodeDir>::Downcast(std::move(out));
            path = nextpath + 1;
        }
    }
}

void BootfsService::PublishStartupVmos(uint8_t type, const char* debug_type_name) {
    constexpr char kVmoSubdir[] = "kernel/";
    constexpr size_t kVmoSubdirLen = sizeof(kVmoSubdir) - 1;

    for (uint16_t i = 0; true; ++i) {
        zx::vmo owned_vmo(zx_take_startup_handle(PA_HND(type, i)));
        if (!owned_vmo.is_valid()) {
            break;
        }
        // We use an unowned VMO here so we can have some finer control over
        // whether the handle is closed.   This is safe since |owned_vmo| will
        // never be closed before |vmo|.
        zx::unowned_vmo vmo(owned_vmo);

        // The first vDSO is the default vDSO.  Since we've taken the startup
        // handle, launchpad won't find it on its own.  So point launchpad at
        // it instead of closing it.
        const bool is_default_vdso = (type == PA_VMO_VDSO && i == 0);
        if (is_default_vdso) {
            launchpad_set_vdso_vmo(owned_vmo.release());
        }

        // The vDSO VMOs have names like "vdso/default", so those
        // become VMO files at "/boot/kernel/vdso/default".
        char name[kVmoSubdirLen + ZX_MAX_NAME_LEN] = {};
        memcpy(name, kVmoSubdir, kVmoSubdirLen);
        size_t size;
        zx_status_t status = vmo->get_property(ZX_PROP_NAME, name + kVmoSubdirLen,
                                               sizeof(name) - kVmoSubdirLen);
        if (status != ZX_OK) {
            printf("bootsvc: vmo.get_property on %s %u: %s\n",
                   debug_type_name, i, zx_status_get_string(status));
            continue;
        }
        status = vmo->get_size(&size);
        if (status != ZX_OK) {
            printf("bootsvc: vmo.get_size on %s %u: %s\n",
                   debug_type_name, i, zx_status_get_string(status));
            continue;
        }
        if (size == 0) {
            // empty vmos do not get installed
            continue;
        }

        if (!strcmp(name + kVmoSubdirLen, "crashlog")) {
            // the crashlog has a special home
            strcpy(name, kLastPanicFilePath);
        }

        if (owned_vmo.is_valid()) {
            status = PublishVmo(name, std::move(owned_vmo), 0, size);
        } else {
            status = PublishUnownedVmo(name, *vmo, 0, size);
        }
        if (status != ZX_OK) {
            printf("bootsvc: failed to add %s %u to filesystem as %s: %s\n",
                   debug_type_name, i, name, zx_status_get_string(status));
            continue;
        }
    }
}

} // namespace bootsvc
