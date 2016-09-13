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

/**
 * As explained in libarchive-3.1.2's debian/README.Debian:
 * > As libarchive requires large-file support enabled under i386 on Linux
 * > you must add the following to any of your own source files:
 * > #define _FILE_OFFSET_BITS 64
 * > Applications compiled under i386 on Linux without this options will
 * > produce empty archives and corrupt output from archives when reading
 * > them.
 * > -- John Goerzen <jgoerzen@complete.org>, Thu,  5 Jun 2008 15:36:22 -0500
 */
#define _FILE_OFFSET_BITS 64

#include "tar/tar-creator.h"

#include <archive.h>
#include <archive_entry.h>

#include <QDebug>
#include <QFile>
#include <QSharedPointer>
#include <QString>

#include <memory>

class TarCreator::Impl
{
public:

    Impl(const QStringList& filenames, bool compress)
        : filenames_(filenames)
        , compress_(compress)
    {
    }

    ssize_t calculate_size() const
    {
        return compress_ ? calculate_compressed_size() : calculate_uncompressed_size();
    }

    bool step(std::vector<char>& fillme)
    {
        step_buf_.resize(0);
        bool success = true;

        // if this is the first step, create an archive
        if (!step_archive_)
        {
            step_archive_.reset(archive_write_new(), [](struct archive* a){archive_write_free(a);});
            archive_write_set_format_pax(step_archive_.get());
            archive_write_set_bytes_per_block(step_archive_.get(), 0);

            if (compress_)
                archive_write_add_filter_xz(step_archive_.get());
            archive_write_open(step_archive_.get(), &step_buf_, nullptr, append_bytes_write_cb, nullptr);

            step_file_.reset();
            step_filenum_ = -1;
        }

        for(;;)
        {
            // if we don't have a file we're working on, then get one
            if (!step_file_)
            {
                if (step_filenum_ >= filenames_.size()) // tried to read past the end
                {
                    success = false;
                    break;
                }

                // step to next file
                if (++step_filenum_ == filenames_.size()) // we made it to the end!
                {
                    wrapped_archive_write_close(step_archive_.get());
                    break;
                }

                // write the file's header
                const auto& filename = filenames_[step_filenum_];
                add_file_header_to_archive(step_archive_.get(), filename);

                // prep it for reading
                step_file_.reset(new QFile(filename));
                step_file_->open(QIODevice::ReadOnly);
            }

            static constexpr int BUFSIZE {1024*10};
            char inbuf[BUFSIZE];
            const auto n = step_file_->read(inbuf, sizeof(inbuf));
            if (n > 0) // got data
            {
                for(;;) {
                    if (archive_write_data(step_archive_.get(), inbuf, size_t(n)) != -1)
                        break;
                    const auto err = archive_errno(step_archive_.get());
                    if (err == ARCHIVE_RETRY)
                        continue;
                    auto errstr = QString::fromUtf8("Error adding data for '%1': %2 (%3)")
                        .arg(step_file_->fileName())
                        .arg(archive_error_string(step_archive_.get()))
                        .arg(err);
                    qWarning() << qPrintable(errstr);
                    if (err != ARCHIVE_WARN)
                        throw std::runtime_error(errstr.toStdString());
                }
            }
            else if (n < 0) // read error
            {
                success = false;
                auto errstr = QStringLiteral("read()ing %1 returned %2 (%3)")
                                  .arg(step_file_->fileName())
                                  .arg(n)
                                  .arg(step_file_->errorString());
                qWarning() << errstr;
                throw std::runtime_error(errstr.toStdString());
            }
            else if (step_file_->atEnd()) // eof
            {
                // loop to next file
                step_file_.reset();
                continue;
            }
        }

        std::swap(fillme,step_buf_);
        return success;
    }

private:

    static ssize_t append_bytes_write_cb(struct archive *,
                                         void * vtarget,
                                         const void * vsource,
                                         size_t len)
    {
        auto& target = *static_cast<std::vector<char>*>(vtarget);
        const auto& source = static_cast<const char*>(vsource);
        target.insert(target.end(), source, source+len);
        return ssize_t(len);
    }

    static ssize_t count_bytes_write_cb(struct archive *,
                                        void * userdata,
                                        const void *,
                                        size_t len)
    {
        *static_cast<ssize_t*>(userdata) += len;
        return ssize_t(len);
    }

    static void add_file_header_to_archive(struct archive* archive,
                                           const QString& filename)
    {
        struct stat st;
        const auto filename_utf8 = filename.toUtf8();
        stat(filename_utf8.constData(), &st);

        auto entry = archive_entry_new();
        archive_entry_copy_stat(entry, &st);
        archive_entry_set_pathname(entry, filename_utf8.constData());

        int ret;
        do {
            ret = archive_write_header(archive, entry);
            if ((ret==ARCHIVE_WARN) || (ret==ARCHIVE_FAILED) || (ret==ARCHIVE_FATAL))
            {
                auto errstr = QString::fromUtf8("Error adding header for '%1': %2 (%3)")
                                .arg(filename)
                                .arg(archive_error_string(archive))
                                .arg(ret);
                qWarning() << qPrintable(errstr);
                if ((ret==ARCHIVE_FATAL) || (ret==ARCHIVE_FAILED))
                    throw std::runtime_error(errstr.toStdString());
            }
        } while (ret == ARCHIVE_RETRY);

        archive_entry_free(entry);
    }

    static void wrapped_archive_write_data(struct archive * archive,
                                           void const     * buf_in,
                                           size_t           bufsize_in,
                                           QString const    source)
    {
        auto bufsize = bufsize_in;
        auto buf = static_cast<char const*>(buf_in);

        while (bufsize > 0)
        {
            auto n_written = archive_write_data(archive, buf, size_t(bufsize));

            if (n_written != -1)
            {
                bufsize -= n_written;
                buf += n_written;
                continue;
            }

            const auto err = archive_errno(archive);
            if (err == ARCHIVE_RETRY)
                continue;

            auto errstr = QString::fromUtf8("Error adding data for '%1': %2 (%3)")
                .arg(source)
                .arg(archive_error_string(archive))
                .arg(err);
            qWarning() << qPrintable(errstr);
            if (err == ARCHIVE_WARN)
                continue;

            throw std::runtime_error(errstr.toStdString());
        }
    }
    static void wrapped_archive_write_close(struct archive* archive)
    {
        for (;;)
        {
            auto const err = archive_write_close(archive);
            if (err == ARCHIVE_OK)
                break;

            if (err == ARCHIVE_RETRY)
                continue;

            auto errstr = QString::fromUtf8("Error calling archive_write_close(): %1 (%2)")
                .arg(archive_error_string(archive))
                .arg(err);
            qWarning() << qPrintable(errstr);
            if (err == ARCHIVE_WARN)
                break;

            throw std::runtime_error(errstr.toStdString());
        }
    }

    ssize_t calculate_uncompressed_size() const
    {
        ssize_t archive_size {};

        auto a = archive_write_new();
        archive_write_set_format_pax(a);
        archive_write_set_bytes_per_block(a, 0);
        archive_write_open(a, &archive_size, nullptr, count_bytes_write_cb, nullptr);

        for (const auto& filename : filenames_)
        {
            add_file_header_to_archive(a, filename);

            // libarchive pads any missing data,
            // so we don't need to call archive_write_data()
        }

        wrapped_archive_write_close(a);
        archive_write_free(a);
        return archive_size;
    }

    ssize_t calculate_compressed_size() const
    {
        ssize_t archive_size {};

        auto a = archive_write_new();
        archive_write_set_format_pax(a);
        archive_write_set_bytes_per_block(a, 0);
        archive_write_add_filter_xz(a);
        archive_write_open(a, &archive_size, nullptr, count_bytes_write_cb, nullptr);

        for (const auto& filename : filenames_)
        {
            add_file_header_to_archive(a, filename);

            // process the file
            QFile file(filename);
            file.open(QIODevice::ReadOnly);
            static constexpr int BUFSIZE {4096};
            char buf[BUFSIZE];
            for(;;) {
                const auto n_read = file.read(buf, sizeof(buf));
                if (n_read == 0)
                    break;
                if (n_read > 0)
                    wrapped_archive_write_data(a, buf, size_t(n_read), filename);
                if (n_read < 0) {
                    auto errstr = QStringLiteral("Reading '%1' returned %2 (%3)")
                                      .arg(file.fileName())
                                      .arg(n_read)
                                      .arg(file.errorString());
                    qCritical() << errstr;
                    throw std::runtime_error(errstr.toStdString());
                }
            }
        }

        wrapped_archive_write_close(a);
        archive_write_free(a);
        return archive_size;
    }

    const QStringList filenames_;
    const bool compress_ {};

    std::vector<char> step_buf_;
    std::shared_ptr<struct archive> step_archive_;
    int step_filenum_ {-1};
    QSharedPointer<QFile> step_file_;
};

/**
***
**/

TarCreator::TarCreator(const QStringList& filenames, bool compress)
    : impl_{new Impl{filenames, compress}}
{
}

TarCreator::~TarCreator() =default;

ssize_t
TarCreator::calculate_size() const
{
    return impl_->calculate_size();
}

bool
TarCreator::step(std::vector<char>& fillme)
{
    return impl_->step(fillme);
}
