/*
 * Copyright (C) 2018, Xilinx Inc - All rights reserved
 * Xilinx SDAccel Media Accelerator API
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */
#ifndef _XMA_HW_H_
#define _XMA_HW_H_

#ifdef __cplusplus
extern "C" {
#endif

//typedef void * XmaKernelRes;
typedef struct XmaHwKernel XmaHwKernel;

typedef struct XmaHwSession
{
    void            *dev_handle;
    //For execbo:
    uint32_t         dev_index;
    int32_t         bank_index;//default bank to use
    XmaHwKernel     *kernel_info;
    uint32_t         reserved[4];
} XmaHwSession;


#ifdef __cplusplus
}
#endif

#endif
