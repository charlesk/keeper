/*
 * Copyright (C) 2016 Canonical, Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *   Charles Kerr <charles.kerr@canonical.com>
 */

#pragma once

#include "util/connection-helper.h"
#include "storage-framework/uploader.h"

#include <unity/storage/qt/client/client-api.h>

#include <QLocalSocket>

#include <memory>

class StorageFrameworkUploader final: public Uploader
{
public:

    StorageFrameworkUploader(std::shared_ptr<unity::storage::qt::client::Uploader> const& uploader,
                             QObject * parent = nullptr);
    std::shared_ptr<QLocalSocket> socket() override;
    void commit() override;
    QString file_name() const override;

private:

    std::shared_ptr<unity::storage::qt::client::Uploader> const uploader_;

    ConnectionHelper connections_;

    QString file_name_after_commit_;
};
