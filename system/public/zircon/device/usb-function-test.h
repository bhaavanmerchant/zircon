// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

// This file contains definitions for the USB function test driver.
// The driver implements three endpoints: bulk out, bulk in and interrupt.
// The driver supports:
//
// 1. Writing data to the device with USB control request.
//
// 2. Reading data back from the device with USB control request.
//
// 3. Requesting the device to send an interrupt packet containing the data
//    sent via USB control request.
//
// 4. Looping back data via the two bulk endpoints.

// USB control request to write data to the device.
#define USB_FUNCTION_TEST_SET_DATA 1

// USB control request to read badk data set by USB_FUNCTION_TEST_SET_DATA.
#define USB_FUNCTION_TEST_GET_DATA 2

// USB control request to request the device to send an interrupt request
// containing the data set via USB_FUNCTION_TEST_SET_DATA.
#define USB_FUNCTION_TEST_SEND_INTERUPT 3

