/* Copyright (c) 2014, 2017, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "Cryptfs_hw"

#include <stdlib.h>
#include <string.h>
#include <sys/limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/qseecom.h>
#include <hardware/keymaster_common.h>
#include <hardware/hardware.h>
#include "cutils/log.h"
#include "cutils/properties.h"
#include "cutils/android_reboot.h"
#include "cryptfs_hw.h"

#define QSEECOM_LIBRARY_NAME "libQSEEComAPI.so"

/*
 * When device comes up or when user tries to change the password, user can
 * try wrong password upto a certain number of times. If user enters wrong
 * password further, HW would wipe all disk encryption related crypto data
 * and would return an error ERR_MAX_PASSWORD_ATTEMPTS to VOLD. VOLD would
 * wipe userdata partition once this error is received.
 */
#define ERR_MAX_PASSWORD_ATTEMPTS			-10
#define MAX_PASSWORD_LEN				32
#define QTI_ICE_STORAGE_UFS				1
#define QTI_ICE_STORAGE_SDCC				2
#define SET_HW_DISK_ENC_KEY				1
#define UPDATE_HW_DISK_ENC_KEY				2

static int loaded_library = 0;
static int (*qseecom_create_key)(int, void*);
static int (*qseecom_update_key)(int, void*, void*);
static int (*qseecom_wipe_key)(int);

#define CRYPTFS_HW_KMS_WIPE_KEY				1
#define CRYPTFS_HW_UP_CHECK_COUNT			100
#define CRYPTFS_HW_KMS_MAX_FAILURE			-10
#define CRYPTFS_HW_UPDATE_KEY_FAILED			-9
#define CRYPTFS_HW_WIPE_KEY_FAILED			-8
#define CRYPTFS_HW_CREATE_KEY_FAILED			-7

#define CRYPTFS_HW_ALGO_MODE_AES_XTS 			0x3

#define METADATA_PARTITION_NAME "/dev/block/bootdevice/by-name/metadata"

enum cryptfs_hw_key_management_usage_type {
	CRYPTFS_HW_KM_USAGE_DISK_ENCRYPTION		= 0x01,
	CRYPTFS_HW_KM_USAGE_FILE_ENCRYPTION		= 0x02,
	CRYPTFS_HW_KM_USAGE_UFS_ICE_DISK_ENCRYPTION	= 0x03,
	CRYPTFS_HW_KM_USAGE_SDCC_ICE_DISK_ENCRYPTION	= 0x04,
	CRYPTFS_HW_KM_USAGE_MAX
};

static inline void* secure_memset(void* v, int c , size_t n)
{
	volatile unsigned char* p = (volatile unsigned char* )v;
	while (n--) *p++ = c;
	return v;
}

static int is_qseecom_up()
{
    int i = 0;
    char value[PROPERTY_VALUE_MAX] = {0};

    for (; i<CRYPTFS_HW_UP_CHECK_COUNT; i++) {
        property_get("sys.keymaster.loaded", value, "");
        if (!strncmp(value, "true", PROPERTY_VALUE_MAX))
            return 1;
        usleep(100000);
    }
    return 0;
}

static int load_qseecom_library()
{
    const char *error = NULL;
    if (loaded_library)
        return loaded_library;

    if (!is_qseecom_up()) {
        SLOGE("Timed out waiting for QSEECom listeners. Aborting FDE key operation");
        return 0;
    }

    void * handle = dlopen(QSEECOM_LIBRARY_NAME, RTLD_NOW);
    if (handle) {
        dlerror(); /* Clear any existing error */
        *(void **) (&qseecom_create_key) = dlsym(handle, "QSEECom_create_key");

        if ((error = dlerror()) == NULL) {
            SLOGD("Success loading QSEECom_create_key \n");
            *(void **) (&qseecom_update_key) = dlsym(handle, "QSEECom_update_key_user_info");
            if ((error = dlerror()) == NULL) {
                SLOGD("Success loading QSEECom_update_key_user_info\n");
                *(void **) (&qseecom_wipe_key) = dlsym(handle, "QSEECom_wipe_key");
                if ((error = dlerror()) == NULL) {
                    loaded_library = 1;
                    SLOGD("Success loading QSEECom_wipe_key \n");
                }
                else
                    SLOGE("Error %s loading symbols for QSEECom APIs \n", error);
            }
            else
                SLOGE("Error %s loading symbols for QSEECom APIs \n", error);
        }
    } else {
        SLOGE("Could not load libQSEEComAPI.so \n");
    }

    if (error)
        dlclose(handle);

    return loaded_library;
}

static int cryptfs_hw_create_key(enum cryptfs_hw_key_management_usage_type usage,
					unsigned char *hash32)
{
	if (load_qseecom_library())
		return qseecom_create_key(usage, hash32);

	return CRYPTFS_HW_CREATE_KEY_FAILED;
}

static int cryptfs_hw_wipe_key(enum cryptfs_hw_key_management_usage_type usage)
{
	if (load_qseecom_library())
		return qseecom_wipe_key(usage);

	return CRYPTFS_HW_WIPE_KEY_FAILED;
}

static int cryptfs_hw_update_key(enum cryptfs_hw_key_management_usage_type usage,
			unsigned char *current_hash32, unsigned char *new_hash32)
{
	if (load_qseecom_library())
		return qseecom_update_key(usage, current_hash32, new_hash32);

	return CRYPTFS_HW_UPDATE_KEY_FAILED;
}

static int map_usage(int usage)
{
    int storage_type = is_ice_enabled();
    if (usage == CRYPTFS_HW_KM_USAGE_DISK_ENCRYPTION) {
        if (storage_type == QTI_ICE_STORAGE_UFS) {
            return CRYPTFS_HW_KM_USAGE_UFS_ICE_DISK_ENCRYPTION;
        }
        else if (storage_type == QTI_ICE_STORAGE_SDCC) {
            return CRYPTFS_HW_KM_USAGE_SDCC_ICE_DISK_ENCRYPTION;
        }
    }
    return usage;
}

static unsigned char* get_tmp_passwd(const char* passwd)
{
    int passwd_len = 0;
    unsigned char * tmp_passwd = NULL;
    if(passwd) {
        tmp_passwd = (unsigned char*)malloc(MAX_PASSWORD_LEN);
        if(tmp_passwd) {
            secure_memset(tmp_passwd, 0, MAX_PASSWORD_LEN);
            passwd_len = strnlen(passwd, MAX_PASSWORD_LEN);
            memcpy(tmp_passwd, passwd, passwd_len);
        } else {
            SLOGE("%s: Failed to allocate memory for tmp passwd \n", __func__);
        }
    } else {
        SLOGE("%s: Passed argument is NULL \n", __func__);
    }
    return tmp_passwd;
}

/*
 * For NON-ICE targets, it would return 0 on success. On ICE based targets,
 * it would return key index in the ICE Key LUT
 */
static int set_key(const char* currentpasswd, const char* passwd, const char* enc_mode, int operation)
{
    int err = -1;
    if (is_hw_disk_encryption(enc_mode)) {
        unsigned char* tmp_passwd = get_tmp_passwd(passwd);
        unsigned char* tmp_currentpasswd = get_tmp_passwd(currentpasswd);
        if (tmp_passwd) {
            if (operation == UPDATE_HW_DISK_ENC_KEY) {
                if (tmp_currentpasswd) {
                   err = cryptfs_hw_update_key(map_usage(CRYPTFS_HW_KM_USAGE_DISK_ENCRYPTION), tmp_currentpasswd, tmp_passwd);
                   secure_memset(tmp_currentpasswd, 0, MAX_PASSWORD_LEN);
                }
            } else if (operation == SET_HW_DISK_ENC_KEY) {
                err = cryptfs_hw_create_key(map_usage(CRYPTFS_HW_KM_USAGE_DISK_ENCRYPTION), tmp_passwd);
            }
            if(err < 0) {
                if(ERR_MAX_PASSWORD_ATTEMPTS == err)
                    SLOGI("Maximum wrong password attempts reached, will erase userdata\n");
            }
            secure_memset(tmp_passwd, 0, MAX_PASSWORD_LEN);
            free(tmp_passwd);
            free(tmp_currentpasswd);
        }
    }
    return err;
}

int set_hw_device_encryption_key(const char* passwd, const char* enc_mode)
{
    return set_key(NULL, passwd, enc_mode, SET_HW_DISK_ENC_KEY);
}

int update_hw_device_encryption_key(const char* oldpw, const char* newpw, const char* enc_mode)
{
    return set_key(oldpw, newpw, enc_mode, UPDATE_HW_DISK_ENC_KEY);
}

unsigned int is_hw_disk_encryption(const char* encryption_mode)
{
    int ret = 0;
    if(encryption_mode) {
        if (!strcmp(encryption_mode, "aes-xts")) {
            SLOGD("HW based disk encryption is enabled \n");
            ret = 1;
        }
    }
    return ret;
}

int is_ice_enabled(void)
{
  char prop_storage[PATH_MAX];
  int storage_type = 0;

  /*
   * Since HW FDE is a compile time flag (due to QSSI requirements),
   * this API conflicts with Metadata encryption even when ICE is
   * enabled, as it encrypts the whole disk instead. Adding this
   * workaround to return 0 if metadata partition is present.
   */

  if (access(METADATA_PARTITION_NAME, F_OK) == 0) {
    SLOGI("Metadata partition, returning false");
    return 0;
  }

  if (property_get("ro.boot.bootdevice", prop_storage, "")) {
    if (strstr(prop_storage, "ufs")) {
      /* All UFS based devices has ICE in it. So we dont need
       * to check if corresponding device exists or not
       */
      storage_type = QTI_ICE_STORAGE_UFS;
    } else if (strstr(prop_storage, "sdhc")) {
      if (access("/dev/icesdcc", F_OK) != -1)
        storage_type = QTI_ICE_STORAGE_SDCC;
    }
  }
  return storage_type;
}

int clear_hw_device_encryption_key()
{
	return cryptfs_hw_wipe_key(map_usage(CRYPTFS_HW_KM_USAGE_DISK_ENCRYPTION));
}

static int get_keymaster_version()
{
    int rc = -1;
    const hw_module_t* mod;
    rc = hw_get_module_by_class(KEYSTORE_HARDWARE_MODULE_ID, NULL, &mod);
    if (rc) {
        SLOGE("could not find any keystore module");
        return rc;
    }

    return mod->module_api_version;
}

int should_use_keymaster()
{
    /*
     * HW FDE key should be tied to keymaster
     * if version is above 0.3. this is to
     * support msm8909 go target.
     */
    int rc = 1;
    if (get_keymaster_version() == KEYMASTER_MODULE_API_VERSION_0_3) {
        SLOGI("Keymaster version is 0.3");
        rc = 0;
    }
    return rc;
}

