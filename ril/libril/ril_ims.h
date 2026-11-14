/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <android/hardware/radio/1.0/types.h>

// Sets up the modem profiles before a data call on the IMS APN (protocol: IP, IPV6...)
void imsBeforeSetupDataCall(const char* apn, const char* protocol);

// Fills the P-CSCF of an IMS data call that qcril does not report
void imsFillPcscf(android::hardware::radio::V1_0::SetupDataCallResult& dcResult);
