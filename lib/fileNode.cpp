////////////////////////////////////////////////////////////////////////////
//  Copyright (C) 2008-2014 by Alexander Galanin                          //
//  al@galanin.nnov.ru                                                    //
//  http://galanin.nnov.ru/~al                                            //
//                                                                        //
//  This program is free software; you can redistribute it and/or modify  //
//  it under the terms of the GNU Lesser General Public License as        //
//  published by the Free Software Foundation; either version 3 of the    //
//  License, or (at your option) any later version.                       //
//                                                                        //
//  This program is distributed in the hope that it will be useful,       //
//  but WITHOUT ANY WARRANTY; without even the implied warranty of        //
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         //
//  GNU General Public License for more details.                          //
//                                                                        //
//  You should have received a copy of the GNU Lesser General Public      //
//  License along with this program; if not, write to the                 //
//  Free Software Foundation, Inc.,                                       //
//  51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA               //
////////////////////////////////////////////////////////////////////////////

#include <cerrno>
#include <climits>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <syslog.h>
#include <cassert>

#include "fileNode.h"
#include "extraField.h"

const zip_int64_t FileNode::ROOT_NODE_INDEX = -1;
const zip_int64_t FileNode::NEW_NODE_INDEX = -2;

FileNode::FileNode(struct zip *zip, const char *fname, zip_int64_t _id) {
    this->zip = zip;
    metadataChanged = false;
    full_name = fname;
    id = _id;
    m_uid = 0;
    m_gid = 0;
}

FileNode *FileNode::createFile (struct zip *zip, const char *fname, 
        uid_t owner, gid_t group, mode_t mode) {
    FileNode *n = new FileNode(zip, fname, NEW_NODE_INDEX);
    if (n == NULL) {
        return NULL;
    }
    n->state = NEW;
    n->is_dir = false;
    n->buffer = new BigBuffer();
    if (!n->buffer) {
        delete n;
        return NULL;
    }
    n->has_cretime = true;
    n->m_mtime = n->m_atime = n->m_ctime = n->cretime = time(NULL);

    n->parse_name();
    n->m_mode = mode;
    n->m_uid = owner;
    n->m_gid = group;

    return n;
}

FileNode *FileNode::createSymlink(struct zip *zip, const char *fname) {
    FileNode *n = new FileNode(zip, fname, NEW_NODE_INDEX);
    if (n == NULL) {
        return NULL;
    }
    n->state = NEW;
    n->is_dir = false;
    n->buffer = new BigBuffer();
    if (!n->buffer) {
        delete n;
        return NULL;
    }
    n->has_cretime = true;
    n->m_mtime = n->m_atime = n->m_ctime = n->cretime = time(NULL);

    n->parse_name();
    n->m_mode = S_IFLNK | 0777;

    return n;
}

/**
 * Create intermediate directory to build full tree
 */
FileNode *FileNode::createIntermediateDir(struct zip *zip,
        const char *fname) {
    FileNode *n = new FileNode(zip, fname, NEW_NODE_INDEX);
    if (n == NULL) {
        return NULL;
    }
    n->state = NEW_DIR;
    n->is_dir = true;
    n->has_cretime = true;
    n->m_mtime = n->m_atime = n->m_ctime = n->cretime = time(NULL);
    n->m_size = 0;
    n->m_mode = S_IFDIR | 0775;

    n->parse_name();

    return n;
}

FileNode *FileNode::createDir(struct zip *zip, const char *fname,
        zip_int64_t id, uid_t owner, gid_t group, mode_t mode) {
    FileNode *n = createNodeForZipEntry(zip, fname, id);
    if (n == NULL) {
        return NULL;
    }
    n->state = CLOSED;
    n->has_cretime = true;
    n->cretime = n->m_mtime;
    // FUSE does not pass S_IFDIR bit here
    n->m_mode = S_IFDIR | mode;
    n->m_uid = owner;
    n->m_gid = group;
    n->is_dir = true;
    return n;
}

FileNode *FileNode::createRootNode() {
    FileNode *n = new FileNode(NULL, "", ROOT_NODE_INDEX);
    if (n == NULL) {
        return NULL;
    }
    n->is_dir = true;
    n->state = NEW_DIR;
    n->m_mtime = n->m_atime = n->m_ctime = n->cretime = time(NULL);
    n->has_cretime = true;
    n->m_size = 0;
    n->name = n->full_name.c_str();
    n->m_mode = S_IFDIR | 0775;
    return n;
}

FileNode *FileNode::createNodeForZipEntry(struct zip *zip,
        const char *fname, zip_int64_t id) {
    FileNode *n = new FileNode(zip, fname, id);
    if (n == NULL) {
        return NULL;
    }
    n->is_dir = false;
    n->open_count = 0;
    n->state = CLOSED;

    struct zip_stat stat;
    zip_stat_index(zip, id, 0, &stat);
    // check that all used fields are valid
    zip_uint64_t needValid = ZIP_STAT_NAME | ZIP_STAT_INDEX |
        ZIP_STAT_SIZE | ZIP_STAT_MTIME;
    // required fields are always valid for existing items or newly added
    // directories (see zip_stat_index.c from libzip)
    assert((stat.valid & needValid) == needValid);
    n->m_mtime = n->m_atime = n->m_ctime = stat.mtime;
    n->has_cretime = false;
    n->m_size = stat.size;

    n->parse_name();

    n->processExternalAttributes();
    n->processExtraFields();
    return n;
}

FileNode::~FileNode() {
    if (state == OPENED || state == CHANGED || state == NEW) {
        delete buffer;
    }
}

/**
 * Get short name of a file. If last char is '/' then node is a directory
 */
void FileNode::parse_name() {
    assert(!full_name.empty());

    const char *lsl = full_name.c_str();
    while (*lsl++) {}
    lsl--;
    while (lsl > full_name.c_str() && *lsl != '/') {
        lsl--;
    }
    // If the last symbol in file name is '/' then it is a directory
    if (*lsl == '/' && *(lsl+1) == '\0') {
        // It will produce two \0s at the end of file name. I think that
        // it is not a problem
        full_name[full_name.size() - 1] = 0;
        this->is_dir = true;
        while (lsl > full_name.c_str() && *lsl != '/') {
            lsl--;
        }
    }
    // Setting short name of node
    if (*lsl == '/') {
        lsl++;
    }
    this->name = lsl;
}

void FileNode::appendChild (FileNode *child) {
    childs.push_back (child);
}

void FileNode::detachChild (FileNode *child) {
    childs.remove (child);
}

void FileNode::rename(const char *new_name) {
    full_name = new_name;
    parse_name();
}

int FileNode::open() {
    if (state == NEW) {
        return 0;
    }
    if (state == OPENED) {
        if (open_count == INT_MAX) {
            return -EMFILE;
        } else {
            ++open_count;
        }
    }
    if (state == CLOSED) {
        open_count = 1;
        try {
            assert (zip != NULL);
            buffer = new BigBuffer(zip, id, m_size);
            state = OPENED;
        }
        catch (std::bad_alloc) {
            return -ENOMEM;
        }
        catch (std::exception) {
            return -EIO;
        }
    }
    return 0;
}

int FileNode::read(char *buf, size_t sz, zip_uint64_t offset) {
    m_atime = time(NULL);
    return buffer->read(buf, sz, offset);
}

int FileNode::write(const char *buf, size_t sz, zip_uint64_t offset) {
    if (state == OPENED) {
        state = CHANGED;
    }
    m_mtime = time(NULL);
    metadataChanged = true;
    return buffer->write(buf, sz, offset);
}

int FileNode::close() {
    m_size = buffer->len;
    if (state == OPENED && --open_count == 0) {
        delete buffer;
        state = CLOSED;
    }
    return 0;
}

int FileNode::save() {
    assert (!is_dir);
    // index is modified if state == NEW
    assert (zip != NULL);
    return buffer->saveToZip(m_mtime, zip, full_name.c_str(),
            state == NEW, id);
}

int FileNode::saveMetadata() const {
    assert(id >= 0);
    return updateExtraFields() && updateExternalAttributes();
}

int FileNode::truncate(zip_uint64_t offset) {
    if (state != CLOSED) {
        if (state != NEW) {
            state = CHANGED;
        }
        try {
            buffer->truncate(offset);
            return 0;
        }
        catch (const std::bad_alloc &) {
            return EIO;
        }
        m_mtime = time(NULL);
        metadataChanged = true;
    } else {
        return EBADF;
    }
}

zip_uint64_t FileNode::size() const {
    if (state == NEW || state == OPENED || state == CHANGED) {
        return buffer->len;
    } else {
        return m_size;
    }
}

/**
 * Get file mode from external attributes.
 */
void FileNode::processExternalAttributes () {
    zip_uint8_t opsys;
    zip_uint32_t attr;
    assert(id >= 0);
    assert (zip != NULL);
    zip_file_get_external_attributes(zip, id, 0, &opsys, &attr);
    switch (opsys) {
        case ZIP_OPSYS_UNIX: {
            m_mode = attr >> 16;
            // force is_dir value
            if (is_dir) {
                m_mode = (m_mode & ~S_IFMT) | S_IFDIR;
            } else {
                m_mode = m_mode & ~S_IFDIR;
            }
            break;
        }
        case ZIP_OPSYS_DOS:
        case ZIP_OPSYS_WINDOWS_NTFS:
        case ZIP_OPSYS_MVS: {
            /*
             * Both WINDOWS_NTFS and OPSYS_MVS used here because of
             * difference in constant assignment by PKWARE and Info-ZIP
             */
            m_mode = 0444;
            // http://msdn.microsoft.com/en-us/library/windows/desktop/gg258117%28v=vs.85%29.aspx
            // http://en.wikipedia.org/wiki/File_Allocation_Table#attributes
            // FILE_ATTRIBUTE_READONLY
            if ((attr & 1) == 0) {
                m_mode |= 0220;
            }
            // directory
            if (is_dir) {
                m_mode |= S_IFDIR | 0111;
            } else {
                m_mode |= S_IFREG;
            }

            break;
        }
        default: {
            if (is_dir) {
                m_mode = S_IFDIR | 0775;
            } else {
                m_mode = S_IFREG | 0664;
            }
        }
    }
}

/**
 * Get timestamp information from extra fields.
 * Get owner and group information.
 */
void FileNode::processExtraFields () {
    zip_int16_t count;
    // times from timestamp have precedence
    bool mtimeFromTimestamp = false, atimeFromTimestamp = false;
    // UIDs and GIDs from UNIX extra fields with bigger type IDs have
    // precedence
    int lastProcessedUnixField = 0;

    assert (id >= 0);
    assert (zip != NULL);
    count = zip_file_extra_fields_count (zip, id, ZIP_FL_LOCAL);
    for (zip_int16_t i = 0; i < count; ++i) {
        bool has_mtime, has_atime, has_cretime;
        time_t mt, at, cret;
        zip_uint16_t type, len;
        const zip_uint8_t *field = zip_file_extra_field_get (zip,
                id, i, &type, &len, ZIP_FL_LOCAL);

        switch (type) {
            case FZ_EF_TIMESTAMP: {
                if (ExtraField::parseExtTimeStamp (len, field, has_mtime, mt,
                            has_atime, at, has_cretime, cret)) {
                    if (has_mtime) {
                        m_mtime = mt;
                        mtimeFromTimestamp = true;
                    }
                    if (has_atime) {
                        m_atime = at;
                        atimeFromTimestamp = true;
                    }
                    if (has_cretime) {
                        cretime = cret;
                        this->has_cretime = true;
                    }
                }
                break;
            }
            case FZ_EF_PKWARE_UNIX:
            case FZ_EF_INFOZIP_UNIX1:
            case FZ_EF_INFOZIP_UNIX2:
            case FZ_EF_INFOZIP_UNIXN: {
                uid_t uid;
                gid_t gid;
                if (ExtraField::parseSimpleUnixField (type, len, field,
                            uid, gid, has_mtime, mt, has_atime, at)) {
                    if (type >= lastProcessedUnixField) {
                        m_uid = uid;
                        m_gid = gid;
                        lastProcessedUnixField = type;
                    }
                    if (has_mtime && !mtimeFromTimestamp) {
                        m_mtime = mt;
                    }
                    if (has_atime && !atimeFromTimestamp) {
                        m_atime = at;
                    }
                }
                break;
            }
        }
    }
}

/**
 * Save timestamp into extra fields
 * @return 0 on success
 */
int FileNode::updateExtraFields () const {
    static zip_flags_t locations[] = {ZIP_FL_CENTRAL, ZIP_FL_LOCAL};

    assert (id >= 0);
    assert (zip != NULL);
    for (unsigned int loc = 0; loc < sizeof(locations); ++loc) {
        // remove old extra fields
        zip_int16_t count = zip_file_extra_fields_count (zip, id,
                locations[loc]);
        const zip_uint8_t *field;
        for (zip_int16_t i = count; i >= 0; --i) {
            zip_uint16_t type;
            field = zip_file_extra_field_get (zip, id, i, &type,
                    NULL, locations[loc]);
            // FZ_EF_PKWARE_UNIX not removed because can contain extra data
            // that currently not handled by fuse-zip
            if (field != NULL && (type == FZ_EF_TIMESTAMP ||
                        type == FZ_EF_INFOZIP_UNIX1 ||
                        type == FZ_EF_INFOZIP_UNIX2 ||
                        type == FZ_EF_INFOZIP_UNIXN)) {
                zip_file_extra_field_delete (zip, id, i,
                        locations[loc]);
            }
        }
        // add new extra fields
        zip_uint16_t len;
        int res;
        // add timestamps
        field = ExtraField::createExtTimeStamp (locations[loc], m_mtime,
                m_atime, has_cretime, cretime, len);
        res = zip_file_extra_field_set (zip, id, FZ_EF_TIMESTAMP,
                ZIP_EXTRA_FIELD_NEW, field, len, locations[loc]);
        if (res != 0) {
            return res;
        }
        // add UNIX owner info
        field = ExtraField::createInfoZipNewUnixField (m_uid, m_gid, len);
        res = zip_file_extra_field_set (zip, id, FZ_EF_INFOZIP_UNIXN,
                ZIP_EXTRA_FIELD_NEW, field, len, locations[loc]);
        if (res != 0) {
            return res;
        }
    }
    return 0;
}

void FileNode::chmod (mode_t mode) {
    m_mode = (m_mode & S_IFMT) | mode;
    m_ctime = time(NULL);
    metadataChanged = true;
}

void FileNode::setUid (uid_t uid) {
    m_uid = uid;
    metadataChanged = true;
}

void FileNode::setGid (gid_t gid) {
    m_gid = gid;
    metadataChanged = true;
}

/**
 * Save OS type and permissions into external attributes
 * @return libzip error code (ZIP_ER_MEMORY or ZIP_ER_RDONLY)
 */
int FileNode::updateExternalAttributes() const {
    assert(id >= 0);
    assert (zip != NULL);
    // save UNIX attributes in high word
    mode_t mode = m_mode << 16;

    // save DOS attributes in low byte
    // http://msdn.microsoft.com/en-us/library/windows/desktop/gg258117%28v=vs.85%29.aspx
    // http://en.wikipedia.org/wiki/File_Allocation_Table#attributes
    if (is_dir) {
        // FILE_ATTRIBUTE_DIRECTORY
        mode |= 0x10;
    }
    if (name[0] == '.') {
        // FILE_ATTRIBUTE_HIDDEN
        mode |= 2;
    }
    if (!(mode & S_IWUSR)) {
        // FILE_ATTRIBUTE_READONLY
        mode |= 1;
    }
    return zip_file_set_external_attributes (zip, id, 0,
            ZIP_OPSYS_UNIX, mode);
}

void FileNode::setTimes (time_t atime, time_t mtime) {
    m_atime = atime;
    m_mtime = mtime;
    metadataChanged = true;
}

void FileNode::setCTime (time_t ctime) {
    m_ctime = ctime;
    metadataChanged = true;
}
