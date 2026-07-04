// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "SceAppUtil.h"

#include <emuenv/app_util.h>

#include <io/device.h>
#include <io/functions.h>
#include <io/io.h>
#include <io/vfs.h>

#include <packages/license.h>
#include <packages/sfo.h>

#include <util/safe_time.h>
#include <util/tracy.h>

#ifdef _WIN32
#include <winsock.h>
#else
#include <unistd.h>
#endif

#include <cstring>
#include <vector>

TRACY_MODULE_NAME(SceAppUtil);

template <>
std::string to_debug_str<SceSystemParamId>(const MemState &mem, SceSystemParamId type) {
    switch (type) {
    case SCE_SYSTEM_PARAM_ID_LANG:
        return "SCE_SYSTEM_PARAM_ID_LANG";
    case SCE_SYSTEM_PARAM_ID_ENTER_BUTTON:
        return "SCE_SYSTEM_PARAM_ID_ENTER_BUTTON";
    case SCE_SYSTEM_PARAM_ID_USER_NAME:
        return "SCE_SYSTEM_PARAM_ID_USER_NAME";
    case SCE_SYSTEM_PARAM_ID_DATE_FORMAT:
        return "SCE_SYSTEM_PARAM_ID_DATE_FORMAT";
    case SCE_SYSTEM_PARAM_ID_TIME_FORMAT:
        return "SCE_SYSTEM_PARAM_ID_TIME_FORMAT";
    case SCE_SYSTEM_PARAM_ID_TIME_ZONE:
        return "SCE_SYSTEM_PARAM_ID_TIME_ZONE";
    case SCE_SYSTEM_PARAM_ID_SUMMERTIME:
        return "SCE_SYSTEM_PARAM_ID_SUMMERTIME";
    case SCE_SYSTEM_PARAM_ID_MAX_VALUE:
        return "SCE_SYSTEM_PARAM_ID_MAX_VALUE";
    }
    return std::to_string(type);
}

EXPORT(int, sceAppUtilAddCookieWebBrowser) {
    TRACY_FUNC(sceAppUtilAddCookieWebBrowser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAddcontMount) {
    TRACY_FUNC(sceAppUtilAddcontMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAddcontUmount) {
    TRACY_FUNC(sceAppUtilAddcontUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseGameCustomData) {
    TRACY_FUNC(sceAppUtilAppEventParseGameCustomData);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseIncomingDialog) {
    TRACY_FUNC(sceAppUtilAppEventParseIncomingDialog);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseLiveArea) {
    TRACY_FUNC(sceAppUtilAppEventParseLiveArea);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNearGift) {
    TRACY_FUNC(sceAppUtilAppEventParseNearGift);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNpActivity) {
    TRACY_FUNC(sceAppUtilAppEventParseNpActivity);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNpAppDataMessage) {
    TRACY_FUNC(sceAppUtilAppEventParseNpAppDataMessage);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNpBasicJoinablePresence) {
    TRACY_FUNC(sceAppUtilAppEventParseNpBasicJoinablePresence);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseNpInviteMessage) {
    TRACY_FUNC(sceAppUtilAppEventParseNpInviteMessage);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseScreenShotNotification) {
    TRACY_FUNC(sceAppUtilAppEventParseScreenShotNotification);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseSessionInvitation) {
    TRACY_FUNC(sceAppUtilAppEventParseSessionInvitation);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseTeleport) {
    TRACY_FUNC(sceAppUtilAppEventParseTeleport);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseTriggerUtil) {
    TRACY_FUNC(sceAppUtilAppEventParseTriggerUtil);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilAppEventParseWebBrowser) {
    TRACY_FUNC(sceAppUtilAppEventParseWebBrowser);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAppUtilAppParamGetInt, SceAppUtilAppParamId paramId, SceInt32 *value) {
    TRACY_FUNC(sceAppUtilAppParamGetInt, paramId, value);
    if (paramId != SCE_APPUTIL_APPPARAM_ID_SKU_FLAG)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (!value)
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_INITIALIZED);

    *value = emuenv.license.rif[emuenv.io.title_id].sku_flag;

    return 0;
}

EXPORT(int, sceAppUtilBgdlGetStatus) {
    TRACY_FUNC(sceAppUtilBgdlGetStatus);
    return UNIMPLEMENTED();
}

static bool is_addcont_exist(EmuEnvState &emuenv, const SceChar8 *path) {
    const auto drm_content_id_path{ emuenv.vita_fs_path / "ux0" / emuenv.io.device_paths.addcont0 / reinterpret_cast<const char *>(path) };
    return (fs::exists(drm_content_id_path) && (!fs::is_empty(drm_content_id_path)));
}

EXPORT(SceInt32, sceAppUtilDrmClose, const SceAppUtilDrmAddcontId *dirName, const SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilDrmClose, dirName, mountPoint);
    if (!dirName)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (!is_addcont_exist(emuenv, dirName->data))
        return RET_ERROR(SCE_APPUTIL_ERROR_NOT_MOUNTED);

    return 0;
}

EXPORT(SceInt32, sceAppUtilDrmOpen, const SceAppUtilDrmAddcontId *dirName, const SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilDrmOpen, dirName, mountPoint);
    if (!dirName)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (!is_addcont_exist(emuenv, dirName->data))
        return SCE_ERROR_ERRNO_ENOENT;

    return 0;
}

EXPORT(int, sceAppUtilInit, void *initParam, void *bootParam) {
    TRACY_FUNC(sceAppUtilInit, initParam, bootParam);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilLaunchWebBrowser) {
    TRACY_FUNC(sceAppUtilLaunchWebBrowser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilMusicMount) {
    TRACY_FUNC(sceAppUtilMusicMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilMusicUmount) {
    TRACY_FUNC(sceAppUtilMusicUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilPhotoMount) {
    TRACY_FUNC(sceAppUtilPhotoMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilPhotoUmount) {
    TRACY_FUNC(sceAppUtilPhotoUmount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilPspSaveDataGetDirNameList) {
    TRACY_FUNC(sceAppUtilPspSaveDataGetDirNameList);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilPspSaveDataLoad) {
    TRACY_FUNC(sceAppUtilPspSaveDataLoad);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilReceiveAppEvent) {
    TRACY_FUNC(sceAppUtilReceiveAppEvent);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilResetCookieWebBrowser) {
    TRACY_FUNC(sceAppUtilResetCookieWebBrowser);
    return UNIMPLEMENTED();
}

std::string construct_savedata0_path(const std::string &data, const char *ext) {
    return device::construct_normalized_path(VitaIoDevice::savedata0, data, ext);
}

std::string construct_slotparam_path(const unsigned int data) {
    return construct_savedata0_path("SlotParam_" + std::to_string(data), "bin");
}

// ---------------------------------------------------------------------------
// sdslot.dat : single-file save slot storage (same layout as a real PS Vita)
//
//   0x000               : header magic "SDSL" + version byte
//                         (the rest of 0x000..0x1FF stays zero)
//   0x200 + slotId      : 1-byte existence flag per slot (0 = empty, 1 = used)
//   0x400 + slotId*0x400: SceAppUtilSaveDataSlotParam (844 bytes) followed by
//                         zero padding up to the 0x400-byte block size
// ---------------------------------------------------------------------------
static constexpr uint32_t SDSLOT_MAX_SLOTS = 256; // fixed slot count (like real Vita)
static constexpr uint32_t SDSLOT_FLAG_BASE = 0x200;
static constexpr uint32_t SDSLOT_DATA_BASE = 0x400;
static constexpr uint32_t SDSLOT_BLOCK_SIZE = 0x400; // 1024 bytes per slot
static constexpr uint32_t SDSLOT_PARAM_SIZE = 844; // sizeof(SceAppUtilSaveDataSlotParam)
// Full file is always written at this fixed size: 0x400 + 256 * 0x400 = 0x40400.
static constexpr size_t SDSLOT_TOTAL_SIZE = SDSLOT_DATA_BASE + static_cast<size_t>(SDSLOT_MAX_SLOTS) * SDSLOT_BLOCK_SIZE;

static const uint8_t SDSLOT_MAGIC[10] = { 'S', 'D', 'S', 'L', 0, 0, 0, 0, 0, 1 };

static_assert(sizeof(SceAppUtilSaveDataSlotParam) == SDSLOT_PARAM_SIZE,
    "SceAppUtilSaveDataSlotParam layout changed; update the sdslot.dat format");

static std::string construct_sdslot_path() {
    return construct_savedata0_path("sdslot", "dat");
}

// Read the whole sdslot.dat into buf. Returns false if it is missing or empty.
static bool sdslot_read_all(EmuEnvState &emuenv, std::vector<uint8_t> &buf, const char *export_name) {
    const auto fd = open_file(emuenv.io, construct_sdslot_path().c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name);
    if (fd < 0)
        return false;
    const SceOff size = seek_file(fd, 0, SCE_SEEK_END, emuenv.io, export_name);
    if (size <= 0) {
        close_file(emuenv.io, fd, export_name);
        return false;
    }
    seek_file(fd, 0, SCE_SEEK_SET, emuenv.io, export_name);
    buf.resize(static_cast<size_t>(size));
    read_file(buf.data(), emuenv.io, fd, static_cast<SceSize>(buf.size()), export_name);
    close_file(emuenv.io, fd, export_name);
    return true;
}

static void sdslot_write_all(EmuEnvState &emuenv, const std::vector<uint8_t> &buf, const char *export_name) {
    const auto fd = open_file(emuenv.io, construct_sdslot_path().c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, emuenv.vita_fs_path, export_name);
    if (fd < 0)
        return;
    write_file(fd, buf.data(), static_cast<SceSize>(buf.size()), emuenv.io, export_name);
    close_file(emuenv.io, fd, export_name);
}

// Normalise buf to the fixed 256-slot full size and (re)stamp the header magic.
static void sdslot_make_full(std::vector<uint8_t> &buf) {
    buf.resize(SDSLOT_TOTAL_SIZE, 0);
    std::memcpy(buf.data(), SDSLOT_MAGIC, sizeof(SDSLOT_MAGIC));
}

// Build a full-size sdslot.dat image in memory from the legacy per-slot
// SlotParam_<n>.bin files. Used for backward compatibility while no sdslot.dat
// exists yet. Returns true if at least one legacy slot was imported.
static bool sdslot_import_legacy(EmuEnvState &emuenv, std::vector<uint8_t> &buf, const char *export_name) {
    sdslot_make_full(buf);
    bool found = false;
    for (uint32_t i = 0; i < SDSLOT_MAX_SLOTS; i++) {
        const auto fd = open_file(emuenv.io, construct_slotparam_path(i).c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name);
        if (fd < 0)
            continue;
        buf[SDSLOT_FLAG_BASE + i] = 1;
        read_file(buf.data() + SDSLOT_DATA_BASE + static_cast<size_t>(i) * SDSLOT_BLOCK_SIZE, emuenv.io, fd, SDSLOT_PARAM_SIZE, export_name);
        close_file(emuenv.io, fd, export_name);
        found = true;
    }
    return found;
}

// Make sure a valid full-size sdslot.dat exists on disk. If it is missing,
// build it once from the legacy per-slot SlotParam_<n>.bin files; if it has an
// unexpected size (older or partial file), normalise it to full size once.
// This full-file pass only ever happens on this one-time creation/migration;
// all regular reads and writes below touch just the requested slot.
static bool sdslot_ensure_file(EmuEnvState &emuenv, const char *export_name) {
    const auto fd = open_file(emuenv.io, construct_sdslot_path().c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name);
    if (fd >= 0) {
        const SceOff size = seek_file(fd, 0, SCE_SEEK_END, emuenv.io, export_name);
        close_file(emuenv.io, fd, export_name);
        if (size == static_cast<SceOff>(SDSLOT_TOTAL_SIZE))
            return true;
        std::vector<uint8_t> buf;
        if (!sdslot_read_all(emuenv, buf, export_name))
            return false;
        sdslot_make_full(buf);
        sdslot_write_all(emuenv, buf, export_name);
        return true;
    }
    std::vector<uint8_t> buf;
    sdslot_import_legacy(emuenv, buf, export_name);
    sdslot_write_all(emuenv, buf, export_name);
    return true;
}

bool sdslot_read_slot_param(EmuEnvState &emuenv, uint32_t slot_id, SceAppUtilSaveDataSlotParam *param, const char *export_name) {
    if (slot_id >= SDSLOT_MAX_SLOTS)
        return false;

    // Fast path: read only this slot's 1-byte flag and its 844-byte block from
    // sdslot.dat instead of loading the whole 256-slot file. The save dialog
    // queries every slot individually (often each frame), so reading the entire
    // file per slot here is extremely slow.
    const auto fd = open_file(emuenv.io, construct_sdslot_path().c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name);
    if (fd >= 0) {
        uint8_t flag = 0;
        seek_file(fd, static_cast<SceOff>(SDSLOT_FLAG_BASE) + slot_id, SCE_SEEK_SET, emuenv.io, export_name);
        read_file(&flag, emuenv.io, fd, sizeof(flag), export_name);
        if (flag == 0) {
            close_file(emuenv.io, fd, export_name);
            return false;
        }
        seek_file(fd, static_cast<SceOff>(SDSLOT_DATA_BASE) + static_cast<SceOff>(slot_id) * SDSLOT_BLOCK_SIZE, SCE_SEEK_SET, emuenv.io, export_name);
        const int n = read_file(param, emuenv.io, fd, SDSLOT_PARAM_SIZE, export_name);
        close_file(emuenv.io, fd, export_name);
        return n == static_cast<int>(SDSLOT_PARAM_SIZE);
    }

    // No sdslot.dat yet: fall back to the legacy SlotParam_<n>.bin for this slot.
    const auto legacy_fd = open_file(emuenv.io, construct_slotparam_path(slot_id).c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name);
    if (legacy_fd < 0)
        return false;
    read_file(param, emuenv.io, legacy_fd, SDSLOT_PARAM_SIZE, export_name);
    close_file(emuenv.io, legacy_fd, export_name);
    return true;
}

static void sdslot_write_slot_param(EmuEnvState &emuenv, uint32_t slot_id, const SceAppUtilSaveDataSlotParam *param, const char *export_name) {
    if (slot_id >= SDSLOT_MAX_SLOTS)
        return;
    if (!sdslot_ensure_file(emuenv, export_name))
        return;

    // Patch only this slot's 1-byte flag and its 844-byte block in place, the
    // same way sdslot_read_slot_param reads, instead of rewriting all 256
    // slots on every save.
    const auto fd = open_file(emuenv.io, construct_sdslot_path().c_str(), SCE_O_WRONLY, emuenv.vita_fs_path, export_name);
    if (fd < 0)
        return;
    const uint8_t flag = 1;
    seek_file(fd, static_cast<SceOff>(SDSLOT_FLAG_BASE) + slot_id, SCE_SEEK_SET, emuenv.io, export_name);
    write_file(fd, &flag, sizeof(flag), emuenv.io, export_name);
    seek_file(fd, static_cast<SceOff>(SDSLOT_DATA_BASE) + static_cast<SceOff>(slot_id) * SDSLOT_BLOCK_SIZE, SCE_SEEK_SET, emuenv.io, export_name);
    write_file(fd, param, SDSLOT_PARAM_SIZE, emuenv.io, export_name);
    close_file(emuenv.io, fd, export_name);
}

static void sdslot_delete_slot(EmuEnvState &emuenv, uint32_t slot_id, const char *export_name) {
    if (slot_id >= SDSLOT_MAX_SLOTS)
        return;
    if (!sdslot_ensure_file(emuenv, export_name))
        return;

    // Clear only this slot's flag and block in place instead of rewriting the
    // whole file.
    const auto fd = open_file(emuenv.io, construct_sdslot_path().c_str(), SCE_O_WRONLY, emuenv.vita_fs_path, export_name);
    if (fd < 0)
        return;
    const uint8_t flag = 0;
    seek_file(fd, static_cast<SceOff>(SDSLOT_FLAG_BASE) + slot_id, SCE_SEEK_SET, emuenv.io, export_name);
    write_file(fd, &flag, sizeof(flag), emuenv.io, export_name);
    const std::vector<uint8_t> zero_block(SDSLOT_BLOCK_SIZE, 0);
    seek_file(fd, static_cast<SceOff>(SDSLOT_DATA_BASE) + static_cast<SceOff>(slot_id) * SDSLOT_BLOCK_SIZE, SCE_SEEK_SET, emuenv.io, export_name);
    write_file(fd, zero_block.data(), SDSLOT_BLOCK_SIZE, emuenv.io, export_name);
    close_file(emuenv.io, fd, export_name);
}

EXPORT(int, sceAppUtilSaveDataDataRemove, SceAppUtilSaveDataFileSlot *slot, SceAppUtilSaveDataRemoveItem *files, unsigned int fileNum, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataDataRemove, slot, files, fileNum, mountPoint);
    for (unsigned int i = 0; i < fileNum; i++) {
        const auto file = fs::path(construct_savedata0_path(files[i].dataPath.get(emuenv.mem)));
        if (fs::is_regular_file(file)) {
            remove_file(emuenv.io, file.string().c_str(), emuenv.vita_fs_path, export_name);
        } else
            remove_dir(emuenv.io, file.string().c_str(), emuenv.vita_fs_path, export_name);
    }

    if (slot && files[0].mode == SCE_APPUTIL_SAVEDATA_DATA_REMOVE_MODE_DEFAULT) {
        sdslot_delete_slot(emuenv, slot->id, export_name);
    }

    return 0;
}

EXPORT(int, sceAppUtilSaveDataDataSave, SceAppUtilSaveDataFileSlot *slot, SceAppUtilSaveDataDataSaveItem *files, unsigned int fileNum, SceAppUtilMountPoint *mountPoint, SceSize *requiredSizeKiB) {
    TRACY_FUNC(sceAppUtilSaveDataDataSave, slot, files, fileNum, mountPoint, requiredSizeKiB);
    SceUID fd;

    if (requiredSizeKiB)
        // requiredSizeKiB must be set to 0 if there is enough space available
        *requiredSizeKiB = 0;

    for (unsigned int i = 0; i < fileNum; i++) {
        const auto file_path = construct_savedata0_path(files[i].dataPath.get(emuenv.mem));
        switch (files[i].mode) {
        case SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_DIRECTORY:
            create_dir(emuenv.io, file_path.c_str(), 0777, emuenv.vita_fs_path, export_name);
            break;
        case SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_FILE_TRUNCATE:
            if (files[i].buf) {
                fd = open_file(emuenv.io, file_path.c_str(), SCE_O_WRONLY | SCE_O_CREAT, emuenv.vita_fs_path, export_name);
                seek_file(fd, static_cast<int>(files[i].offset), SCE_SEEK_SET, emuenv.io, export_name);
                write_file(fd, files[i].buf.get(emuenv.mem), files[i].bufSize, emuenv.io, export_name);
                close_file(emuenv.io, fd, export_name);
            }
            fd = open_file(emuenv.io, file_path.c_str(), SCE_O_WRONLY | SCE_O_APPEND | SCE_O_TRUNC, emuenv.vita_fs_path, export_name);
            truncate_file(fd, files[i].bufSize + files[i].offset, emuenv.io, export_name);
            close_file(emuenv.io, fd, export_name);
            break;
        case SCE_APPUTIL_SAVEDATA_DATA_SAVE_MODE_FILE:
        default:
            fd = open_file(emuenv.io, file_path.c_str(), SCE_O_WRONLY | SCE_O_CREAT, emuenv.vita_fs_path, export_name);
            seek_file(fd, static_cast<int>(files[i].offset), SCE_SEEK_SET, emuenv.io, export_name);
            write_file(fd, files[i].buf.get(emuenv.mem), files[i].bufSize, emuenv.io, export_name);
            close_file(emuenv.io, fd, export_name);
            break;
        }
    }

    if (slot) {
        SceAppUtilSaveDataSlotParam param{};
        auto *slot_param = slot->slotParam ? slot->slotParam.get(emuenv.mem) : &param;
        SceDateTime modified_time;
        std::time_t time = std::time(0);
        tm local = {};

        if (!slot->slotParam) {
            if (!sdslot_read_slot_param(emuenv, slot->id, slot_param, export_name))
                return 0;
        }

        SAFE_LOCALTIME(&time, &local);
        modified_time.year = local.tm_year + 1900;
        modified_time.month = local.tm_mon + 1;
        modified_time.day = local.tm_mday;
        modified_time.hour = local.tm_hour;
        modified_time.minute = local.tm_min;
        modified_time.second = local.tm_sec;
        slot_param->modifiedTime = modified_time;
        sdslot_write_slot_param(emuenv, slot->id, slot_param, export_name);
    }

    return 0;
}

EXPORT(int, sceAppUtilSaveDataGetQuota, SceSize *quotaSizeKiB, SceSize *usedSizeKiB, const SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataGetQuota, quotaSizeKiB, usedSizeKiB, mountPoint);

    if (!quotaSizeKiB && !usedSizeKiB)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    // Quota from SFO
    if (quotaSizeKiB) {
        std::string savedata_max_size;

        if (!sfo::get_data_by_key(savedata_max_size, emuenv.sfo_handle, "SAVEDATA_MAX_SIZE"))
            savedata_max_size = "0";

        *quotaSizeKiB = static_cast<SceSize>(std::strtoul(savedata_max_size.c_str(), nullptr, 10));
    }

    // Used size from VFS
    if (usedSizeKiB) {
        *usedSizeKiB = vfs::get_directory_used_size(VitaIoDevice::ux0, emuenv.io.device_paths.savedata0, emuenv.vita_fs_path) / KiB(1);

        // Clamp used size to quota
        if (quotaSizeKiB && (*quotaSizeKiB > 0) && (*usedSizeKiB > *quotaSizeKiB))
            *usedSizeKiB = *quotaSizeKiB;
    }

    return 0;
}

EXPORT(int, sceAppUtilSaveDataMount) {
    TRACY_FUNC(sceAppUtilSaveDataMount);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilSaveDataSlotCreate, unsigned int slotId, SceAppUtilSaveDataSlotParam *param, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotCreate, slotId, param, mountPoint);
    sdslot_write_slot_param(emuenv, slotId, param, export_name);
    return 0;
}

EXPORT(int, sceAppUtilSaveDataSlotDelete, unsigned int slotId, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotDelete, slotId, mountPoint);
    sdslot_delete_slot(emuenv, slotId, export_name);
    return 0;
}

EXPORT(int, sceAppUtilSaveDataSlotGetParam, unsigned int slotId, SceAppUtilSaveDataSlotParam *param, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotGetParam, slotId, param, mountPoint);
    if (!sdslot_read_slot_param(emuenv, slotId, param, export_name))
        return RET_ERROR(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    param->status = 0;
    return 0;
}

EXPORT(SceInt32, sceAppUtilSaveDataSlotSearch, SceAppUtilWorkBuffer *workBuf, const SceAppUtilSaveDataSlotSearchCond *cond,
    SceAppUtilSlotSearchResult *result, const SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotSearch, workBuf, cond, result, mountPoint);
    STUBBED("No sort slot list");

    if (!cond || !result)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    if (workBuf)
        result->slotList = Ptr<SceAppUtilSaveDataSlot>(workBuf->buf.address());

    result->hitNum = 0;
    auto slotList = result->slotList.get(emuenv.mem);
    for (auto i = cond->from; i < (cond->from + cond->range); i++) {
        if (slotList) {
            slotList[i].id = -1;
            slotList[i].status = 0;
            slotList[i].userParam = 0;
            slotList[i].emptyParam = Ptr<SceAppUtilSaveDataSlotEmptyParam>(0);
        }

        SceAppUtilSaveDataSlotParam param{};
        const bool exist = sdslot_read_slot_param(emuenv, i, &param, export_name);
        switch (cond->type) {
        case SCE_APPUTIL_SAVEDATA_SLOT_SEARCH_TYPE_EXIST_SLOT:
            if (exist) {
                if (slotList) {
                    slotList[result->hitNum].userParam = param.userParam;
                    slotList[result->hitNum].status = param.status;
                    slotList[result->hitNum].id = i;
                }
                result->hitNum++;
            }
            break;
        case SCE_APPUTIL_SAVEDATA_SLOT_SEARCH_TYPE_EMPTY_SLOT:
            if (!exist) {
                if (slotList)
                    slotList[result->hitNum].id = i;
                result->hitNum++;
            }
            break;
        default: break;
        }
    }

    return 0;
}

EXPORT(SceInt32, sceAppUtilSaveDataSlotSetParam, SceAppUtilSaveDataSlotId slotId, SceAppUtilSaveDataSlotParam *param, SceAppUtilMountPoint *mountPoint) {
    TRACY_FUNC(sceAppUtilSaveDataSlotSetParam, slotId, param, mountPoint);
    SceAppUtilSaveDataSlotParam existing{};
    if (!sdslot_read_slot_param(emuenv, slotId, &existing, export_name))
        return RET_ERROR(SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND);
    sdslot_write_slot_param(emuenv, slotId, param, export_name);
    return 0;
}

EXPORT(int, sceAppUtilSaveDataUmount) {
    TRACY_FUNC(sceAppUtilSaveDataUmount);
    return UNIMPLEMENTED();
}

static SceInt32 SafeMemory(EmuEnvState &emuenv, void *buf, SceSize bufSize, SceOff offset, const char *export_name, bool save) {
    std::vector<char> safe_mem(SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE);
    const auto safe_mem_path = construct_savedata0_path("sce_sys/safemem", "dat");
    SceInt32 res = 0;

    // Open file when it exist
    const auto fd = open_file(emuenv.io, safe_mem_path.c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, export_name);
    if (fd > 0) {
        // Read file for set data inside safe mem when it exist
        res = read_file(safe_mem.data(), emuenv.io, fd, SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE, export_name);
        close_file(emuenv.io, fd, export_name);
    }

    if ((fd < 0) || save) {
        // When safe mem no exist or in save mode, write it with set buffer inside data
        const auto fd = open_file(emuenv.io, safe_mem_path.c_str(), SCE_O_WRONLY | SCE_O_CREAT, emuenv.vita_fs_path, export_name);
        memcpy(&safe_mem[offset], buf, bufSize);
        write_file(fd, safe_mem.data(), SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE, emuenv.io, export_name);
        close_file(emuenv.io, fd, export_name);
    } else
        memcpy(buf, &safe_mem[offset], bufSize);

    return res;
}

EXPORT(SceInt32, sceAppUtilLoadSafeMemory, void *buf, SceSize bufSize, SceOff offset) {
    TRACY_FUNC(sceAppUtilLoadSafeMemory, buf, bufSize, offset);
    if (!buf || (offset + bufSize > SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    const auto res = SafeMemory(emuenv, buf, bufSize, offset, export_name, false);

    // Load can return 0 when file no exist
    return res > 0 ? bufSize : 0;
}

EXPORT(SceInt32, sceAppUtilSaveSafeMemory, const void *buf, SceSize bufSize, SceOff offset) {
    TRACY_FUNC(sceAppUtilSaveSafeMemory, buf, bufSize, offset);
    if (!buf || (offset + bufSize > SCE_APPUTIL_SAFEMEMORY_MEMORY_SIZE))
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    SafeMemory(emuenv, const_cast<void *>(buf), bufSize, offset, export_name, true);

    return bufSize;
}

EXPORT(int, sceAppUtilShutdown) {
    TRACY_FUNC(sceAppUtilShutdown);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAppUtilStoreBrowse) {
    TRACY_FUNC(sceAppUtilStoreBrowse);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAppUtilSystemParamGetInt, SceSystemParamId paramId, SceInt32 *value) {
    TRACY_FUNC(sceAppUtilSystemParamGetInt, paramId, value);
    if (!value)
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);

    switch (paramId) {
    case SCE_SYSTEM_PARAM_ID_LANG:
        *value = (SceSystemParamLang)emuenv.cfg.sys_lang;
        return 0;
    case SCE_SYSTEM_PARAM_ID_ENTER_BUTTON:
        *value = (SceSystemParamEnterButtonAssign)emuenv.cfg.sys_button;
        return 0;
    case SCE_SYSTEM_PARAM_ID_DATE_FORMAT:
        *value = (SceSystemParamDateFormat)emuenv.cfg.sys_date_format;
        return 0;
    case SCE_SYSTEM_PARAM_ID_TIME_FORMAT:
        *value = (SceSystemParamTimeFormat)emuenv.cfg.sys_time_format;
        return 0;
    case SCE_SYSTEM_PARAM_ID_TIME_ZONE:
    case SCE_SYSTEM_PARAM_ID_SUMMERTIME:
        STUBBED("No support Time Zone and Summer Time, give 0 value");
        *value = 0;
        return 0;
    default:
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    }
}

EXPORT(int, sceAppUtilSystemParamGetString, unsigned int paramId, SceChar8 *buf, SceSize bufSize) {
    TRACY_FUNC(sceAppUtilSystemParamGetString, paramId, buf, bufSize);
    constexpr auto devname_len = SCE_SYSTEM_PARAM_USERNAME_MAXSIZE;
    char devname[devname_len];
    switch (paramId) {
    case SCE_SYSTEM_PARAM_ID_USER_NAME:
        if (gethostname(devname, devname_len)) {
            // fallback to User Name
            std::strncpy(devname, emuenv.io.user_name.c_str(), sizeof(devname));
        }
        std::strncpy(reinterpret_cast<char *>(buf), devname, sizeof(devname));
        break;
    default:
        return RET_ERROR(SCE_APPUTIL_ERROR_PARAMETER);
    }
    return 0;
}
