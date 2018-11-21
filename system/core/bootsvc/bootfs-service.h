// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>
#include <fbl/vector.h>
#include <lib/async/dispatcher.h>
#include <lib/memfs/cpp/vnode.h>
#include <lib/zx/vmo.h>

namespace bootsvc {

class BootfsService : public fbl::RefCounted<BootfsService> {
public:
    ~BootfsService();

    // Create an empty BootfsService and set up its VFS to use the given async dispatcher.
    static zx_status_t Create(async_dispatcher_t* dispatcher, fbl::RefPtr<BootfsService>* out);

    // Overlays the contents of |bootfs| on top of the existing VFS.
    zx_status_t AddBootfs(zx::vmo bootfs);

    // Creates a connection to the root of the bootfs VFS and returns
    // a channel that can be used to speak the fuchsia.io.Node interface.
    zx_status_t CreateRootConnection(zx::channel* out);

    // Looks up the given path in the bootfs and returns its contents and size.
    zx_status_t Open(const char* path, zx::vmo* vmo, size_t* size);

    // Publishes the given |vmo| range into the bootfs at |path|.  |path| should
    // not begin with a slash and be relative to the root of the bootfs.
    zx_status_t PublishVmo(const char* path, zx::vmo vmo, zx_off_t off, size_t len);

    // Same as PublishVmo, but the caller must ensure that |vmo| outlives the
    // bootfs service.
    zx_status_t PublishUnownedVmo(const char* path, const zx::vmo& vmo, zx_off_t off, size_t len);

    // Publishes all of the VMOs from the startup handles table with the given
    // |type|.  |debug_type_name| is used for debug printing.
    void PublishStartupVmos(uint8_t type, const char* debug_type_name);
private:
    BootfsService() = default;

    // owned_vmos contains all VMOs that are claimed by the underlying VFS
    fbl::Vector<zx::vmo> owned_vmos_;

    memfs::Vfs vfs_;
    // root of the vfs
    fbl::RefPtr<memfs::VnodeDir> root_;
};

} // namespace bootsvc
