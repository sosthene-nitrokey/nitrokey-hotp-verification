/*
 * Copyright (c) 2023 Nitrokey GmbH
 *
 * This file is part of Nitrokey HOTP verification project.
 *
 * Nitrokey HOTP verification is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * Nitrokey HOTP verification is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Nitrokey HOTP verification. If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0
 */

#include "operations_ccid.h"
#include "base32.h"
#include "ccid.h"
#include "device.h"
#include "return_codes.h"
#include "settings.h"
#include "tlv.h"
#include "utils.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>


int set_pin_ccid(struct Device *dev, const char *admin_PIN) {
    TLV tlvs[] = {
            {
                    .tag = Tag_Password,
                    .length = strnlen(admin_PIN, MAX_PIN_SIZE_CCID),
                    .type = 'S',
                    .v_str = admin_PIN,
            },
    };

    clean_buffers(dev);
    // encode
    uint32_t icc_actual_length = icc_pack_tlvs_for_sending(dev->ccid_buffer_out, sizeof dev->ccid_buffer_out,
                                                           tlvs, ARR_LEN(tlvs), Ins_SetPIN);

    // send
    IccResult iccResult;
    int r = ccid_process_single(dev->mp_devhandle_ccid, dev->ccid_buffer_in, sizeof dev->ccid_buffer_in,
                                dev->ccid_buffer_out, icc_actual_length, &iccResult);

    if (r != 0) {
        return r;
    }
    // check status code
    if (iccResult.data_status_code != 0x9000) {
        return 1;
    }

    return 0;
}


int authenticate_ccid(struct Device *dev, const char *admin_PIN) {
    TLV tlvs[] = {
            {
                    .tag = Tag_Password,
                    .length = strnlen(admin_PIN, 30),
                    .type = 'S',
                    .v_str = admin_PIN,
            },
    };

    clean_buffers(dev);
    // encode
    uint32_t icc_actual_length = icc_pack_tlvs_for_sending(dev->ccid_buffer_out, sizeof dev->ccid_buffer_out,
                                                           tlvs, ARR_LEN(tlvs), Ins_VerifyPIN);
    // send
    IccResult iccResult;
    int r = ccid_process_single(dev->mp_devhandle_ccid, dev->ccid_buffer_in, sizeof dev->ccid_buffer_in,
                                dev->ccid_buffer_out, icc_actual_length, &iccResult);
    if (r != 0) {
        return r;
    }

    // check status code
    if (iccResult.data_status_code == 0x6300) {
        // Invalid PIN or PIN attempt counter is used up
        return RET_WRONG_PIN;
    }
    if (iccResult.data_status_code != 0x9000) {
        // TODO print the error code
        return 1;
    }

    return RET_NO_ERROR;
}


int set_secret_on_device_ccid(struct Device *dev, const char *OTP_secret_base32, const uint64_t hotp_counter) {
    // Decode base32 secret
    uint8_t binary_secret_buf[HOTP_SECRET_SIZE_BYTES + 2] = {0};
    const size_t decoded_length = base32_decode((const unsigned char *) OTP_secret_base32, binary_secret_buf + 2) + 2;
    rassert(decoded_length <= HOTP_SECRET_SIZE_BYTES);

    binary_secret_buf[0] = Kind_HotpReverse | Algo_Sha1;
    binary_secret_buf[1] = (HOTP_CODE_USE_8_DIGITS) ? 8 : 6;

    // 0x02 if touch_button_required else 0x00
    uint8_t properties[2] = {Tag_Properties, 0x00};

    rassert(hotp_counter < 0xFFFFFFFF);
    uint32_t initial_counter_value = hotp_counter;

    TLV tlvs[] = {
            {
                    .tag = Tag_CredentialId,
                    .length = SLOT_NAME_LEN,
                    .type = 'S',
                    .v_str = SLOT_NAME,
            },
            {
                    .tag = Tag_Key,
                    .length = decoded_length,
                    .type = 'R',
                    .v_data = binary_secret_buf,
            },
            {
                    .tag = Tag_Properties,
                    .length = 2,
                    .type = 'B',
                    .v_data = properties,
            },
            {
                    .tag = Tag_InitialCounter,
                    .length = 4,
                    .type = 'I',
                    .v_raw = initial_counter_value,
            },
    };


    clean_buffers(dev);
    // encode
    uint32_t icc_actual_length = icc_pack_tlvs_for_sending(dev->ccid_buffer_out, sizeof dev->ccid_buffer_out,
                                                           tlvs, ARR_LEN(tlvs), Ins_Put);

    // send
    IccResult iccResult;
    int r = ccid_process_single(dev->mp_devhandle_ccid, dev->ccid_buffer_in, sizeof dev->ccid_buffer_in,
                                dev->ccid_buffer_out, icc_actual_length, &iccResult);


    if (r != 0) {
        return r;
    }
    // check status code
    if (iccResult.data_status_code == 0x6a82) {
        return RET_NO_PIN_ATTEMPTS;
    }
    if (iccResult.data_status_code == 0x6982) {
        return RET_SECURITY_STATUS_NOT_SATISFIED;
    }
    if (iccResult.data_status_code != 0x9000) {
        return RET_VALIDATION_FAILED;
    }

    return RET_NO_ERROR;
}

int verify_code_ccid(struct Device *dev, const uint32_t code_to_verify) {
    int r;

    TLV tlvs[] = {
            {
                    .tag = Tag_CredentialId,
                    .length = SLOT_NAME_LEN,
                    .type = 'S',
                    .v_str = SLOT_NAME,
            },
            {
                    .tag = Tag_Response,
                    .length = 4,
                    .type = 'I',
                    .v_raw = code_to_verify,
            },
    };


    clean_buffers(dev);
    // encode
    uint32_t icc_actual_length = icc_pack_tlvs_for_sending(dev->ccid_buffer_out, sizeof dev->ccid_buffer_out,
                                                           tlvs, ARR_LEN(tlvs), Ins_VerifyCode);

    // send
    IccResult iccResult;
    r = ccid_process_single(dev->mp_devhandle_ccid, dev->ccid_buffer_in, sizeof dev->ccid_buffer_in,
                            dev->ccid_buffer_out, icc_actual_length, &iccResult);
    if (r != 0) {
        return r;
    }
    // check status code
    if (iccResult.data_status_code == 0x6A82) {
        // Slot is not configured or requires PIN to proceed. Ask User for the latter.
        return RET_SLOT_NOT_CONFIGURED;
    }

    if (iccResult.data_status_code != 0x9000) {
        return RET_VALIDATION_FAILED;
    }

    return RET_VALIDATION_PASSED;
}

int status_ccid(libusb_device_handle *handle, int *attempt_counter, uint16_t *firmware_version, uint32_t *serial_number) {
    rassert(handle != NULL);
    rassert(attempt_counter != NULL);
    rassert(firmware_version != NULL);
    rassert(serial_number != NULL);
    uint8_t buf[1024] = {};
    IccResult iccResult = {};
    int r = send_select_ccid(handle, buf, sizeof buf, &iccResult);
    if (r != RET_NO_ERROR) {
        return r;
    }
    if (iccResult.data_len == 0 || iccResult.data_status_code != 0x9000) {
        return RET_COMM_ERROR;
    }

    TLV counter_tlv = {};
    r = get_tlv(iccResult.data, iccResult.data_len, Tag_PINCounter, &counter_tlv);
    if (!(r == RET_NO_ERROR && counter_tlv.tag == Tag_PINCounter)) {
        // PIN counter not found - comm error (ignore) or PIN not set
        *attempt_counter = -1;
    } else {
        *attempt_counter = counter_tlv.v_data[0];
    }

    TLV serial_tlv = {};
    r = get_tlv(iccResult.data, iccResult.data_len, Tag_SerialNumber, &serial_tlv);
    if (r == RET_NO_ERROR && serial_tlv.tag == Tag_SerialNumber) {
        *serial_number = be32toh(*(uint32_t *) serial_tlv.v_data);
    } else {
        // ignore errors - unsupported or hidden serial_tlv number
        *serial_number = 0;
    }

    TLV version_tlv = {};
    r = get_tlv(iccResult.data, iccResult.data_len, Tag_Version, &version_tlv);
    if (!(r == RET_NO_ERROR && version_tlv.tag == Tag_Version)) {
        *firmware_version = 0;
        return RET_COMM_ERROR;
    }
    *firmware_version = be16toh(*(uint16_t *) version_tlv.v_data);

    if (*attempt_counter == -1) {
        return RET_NO_PIN_ATTEMPTS;
    }
    return RET_NO_ERROR;
}
