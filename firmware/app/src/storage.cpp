#include "storage.h"

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr size_t kMaxBlob = 64;
constexpr int kBlobCount = 4;
constexpr int kLegacyBlobCount = 3;
constexpr uint16_t kLegacyIds[kBlobCount] = {1, 2, 3, 4};

struct Blob {
	uint16_t id;
	uint8_t data[kMaxBlob];
	int len; // -1 = saknas
};

Blob g_blobs[kBlobCount];
bool g_ready;
bool g_migrated;

Blob *Find(uint16_t id)
{
	for (Blob &blob : g_blobs) {
		if (blob.id == id) {
			return &blob;
		}
	}
	return nullptr;
}

int OnSetting(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	(void)param;

	Blob *blob = Find(static_cast<uint16_t>(strtoul(key, nullptr, 10)));

	if (blob == nullptr || len > kMaxBlob) {
		return 0;
	}

	const ssize_t read = read_cb(cb_arg, blob->data, len);

	blob->len = read > 0 ? static_cast<int>(read) : -1;
	return 0;
}

// Läser gamla råa NVS-poster (3 sektorer från partitionens början) innan settings tar över.
void ReadLegacy(Blob *legacy)
{
	const struct flash_area *area;

	if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &area)) {
		return;
	}

	nvs_fs fs = {};
	struct flash_pages_info info;

	fs.flash_device = area->fa_dev;
	fs.offset = area->fa_off;
	const int err = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);

	flash_area_close(area);
	if (err) {
		return;
	}

	fs.sector_size = static_cast<uint16_t>(info.size);
	fs.sector_count = 3;
	if (nvs_mount(&fs)) {
		return;
	}

	for (int i = 0; i < kLegacyBlobCount; i++) {
		const ssize_t read = nvs_read(&fs, legacy[i].id, legacy[i].data, kMaxBlob);

		legacy[i].len = read > 0 ? static_cast<int>(read) : -1;
	}
}

} // namespace

int StorageInit()
{
	if (g_ready) {
		return 0;
	}

	Blob legacy[kBlobCount];

	for (int i = 0; i < kBlobCount; i++) {
		g_blobs[i] = {kLegacyIds[i], {}, -1};
	}
	for (int i = 0; i < kLegacyBlobCount; i++) {
		legacy[i] = {kLegacyIds[i], {}, -1};
	}

	ReadLegacy(legacy);

	int err = settings_subsys_init();

	if (err) {
		return err;
	}

	err = settings_load_subtree_direct("nrfnode", OnSetting, nullptr);
	if (err) {
		return err;
	}

	g_ready = true;

	for (int i = 0; i < kLegacyBlobCount; i++) {
		if (g_blobs[i].len < 0 && legacy[i].len > 0) {
			memcpy(g_blobs[i].data, legacy[i].data, static_cast<size_t>(legacy[i].len));
			g_blobs[i].len = legacy[i].len;
			if (StorageWrite(g_blobs[i].id, g_blobs[i].data,
					 static_cast<size_t>(g_blobs[i].len)) == 0) {
				g_migrated = true;
			}
		}
	}

	return 0;
}

bool StorageMigrated()
{
	return g_migrated;
}

int StorageRead(uint16_t id, void *data, size_t len)
{
	const Blob *blob = Find(id);

	if (!g_ready || blob == nullptr || blob->len < 0) {
		return -ENOENT;
	}

	const size_t n = static_cast<size_t>(blob->len) < len ? static_cast<size_t>(blob->len) : len;

	memcpy(data, blob->data, n);
	return static_cast<int>(n);
}

int StorageWrite(uint16_t id, const void *data, size_t len)
{
	Blob *blob = Find(id);

	if (!g_ready || blob == nullptr || len > kMaxBlob) {
		return -ENODEV;
	}

	char key[16];

	snprintf(key, sizeof(key), "nrfnode/%u", static_cast<unsigned int>(id));

	const int err = settings_save_one(key, data, len);

	if (err) {
		return err;
	}

	memcpy(blob->data, data, len);
	blob->len = static_cast<int>(len);
	return 0;
}
