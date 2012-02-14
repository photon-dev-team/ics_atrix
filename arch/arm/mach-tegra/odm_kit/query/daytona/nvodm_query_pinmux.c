/*
 * Copyright (c) 2009 NVIDIA Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NVIDIA Corporation nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * This file implements the pin-mux configuration tables for each I/O module.
 */

// THESE SETTINGS ARE PLATFORM-SPECIFIC (not SOC-specific).
// PLATFORM = AP20 Motorola Etna

#include "nvodm_query_pinmux.h"
#include "nvassert.h"
#include "nvodm_services.h"
#include "tegra_devkit_custopt.h"
#include "nvodm_keylist_reserved.h"
#include "nvrm_drf.h"

#define NVODM_PINMUX_ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))


static const NvU32 s_NvOdmPinMuxConfig_Uart[] = {
    NvOdmUartPinMap_Config4,
    NvOdmUartPinMap_Config2,
    NvOdmUartPinMap_Config1,
    NvOdmUartPinMap_Config2,
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_Spi[] = {
    NvOdmSpiPinMap_Config4,
    NvOdmSpiPinMap_Config2,
    NvOdmSpiPinMap_Config2,
    0,
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_Twc[] = {
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_I2c[] = {
    NvOdmI2cPinMap_Config1,
    NvOdmI2cPinMap_Config1,
    NvOdmI2cPinMap_Config1,
};

static const NvU32 s_NvOdmPinMuxConfig_I2c_Pmu[] = {
    NvOdmI2cPmuPinMap_Config1,
};

static const NvU32 s_NvOdmPinMuxConfig_Ulpi[] = {
    NvOdmUlpiPinMap_Config1,
};

static const NvU32 s_NvOdmPinMuxConfig_Sdio[] = {
    NvOdmSdioPinMap_Config1,    /* Wifi */
    0,
    NvOdmSdioPinMap_Config2,
    NvOdmSdioPinMap_Config2,
};

static const NvU32 s_NvOdmPinMuxConfig_Spdif[] = {
    NvOdmSdioPinMap_Config2,
};

static const NvU32 s_NvOdmPinMuxConfig_Hsi[] = {
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_Hdmi[] = {
    NvOdmHdmiPinMap_Config1,
};

static const NvU32 s_NvOdmPinMuxConfig_Pwm[] = {
    NvOdmPwmPinMap_Config1,
};

static const NvU32 s_NvOdmPinMuxConfig_Ata[] = {
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_Nand[] = {
    0,
};

///TODO: keep it here for now and might remove it.
static const NvU32 s_NvOdmPinMuxConfig_Dsi[] = {
    NvOdmDapPinMap_Config1, // fake one, otherwise, ddk display will assert.
};

static const NvU32 s_NvOdmPinMuxConfig_Dap[] = {
    NvOdmDapPinMap_Config1,
    NvOdmDapPinMap_Config1,
    NvOdmDapPinMap_Config1,
    NvOdmDapPinMap_Config1,
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_Kbd[] = {
    NvOdmKbdPinMap_Config3,
};

static const NvU32 s_NvOdmPinMuxConfig_Hdcp[] = {
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_SyncNor[] = {
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_Mio[] = {
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_ExternalClock[] = {
    NvOdmExternalClockPinMap_Config2,
    NvOdmExternalClockPinMap_Config2,
    NvOdmExternalClockPinMap_Config1, // CSUS -> VI_Sensor_CLK
};

static const NvU32 s_NvOdmPinMuxConfig_VideoInput[] = {
    NvOdmVideoInputPinMap_Config2,
};

static const NvU32 s_NvOdmPinMuxConfig_Display[] = {
    0,
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_BacklightPwm[] = {
    0,
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_Crt[] = {
    0,
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_Tvo[] = {
    0,
};

static const NvU32 s_NvOdmPinMuxConfig_OneWire[] = {
    NvOdmOneWirePinMap_Config1,
};

static const NvU32 s_NvOdmPinMuxConfig_PciExpress[] = {
    0, // To enable Pcie, set pinmux config for SDIO3 to 0
};

static const NvU32 s_NvOdmClockLimit_Sdio[] = {
    50000,// WLAN Dima temporary speed limit for Whistler
    32000,
    50000,
    50000,
};

static const NvU32 s_NvOdmPinMuxConfig_Ptm[] = {
    NvOdmPtmPinMap_Config1,
};

void
NvOdmQueryPinMux(
    NvOdmIoModule IoModule,
    const NvU32 **pPinMuxConfigTable,
    NvU32 *pCount)
{
    NvU32 CustomerOption = 0;
    NvU32 Personality = 0;
    NvU32 Ril = 0;
    NvOdmServicesKeyListHandle hKeyList;

    hKeyList = NvOdmServicesKeyListOpen();
    if (hKeyList)
    {
        CustomerOption =
            NvOdmServicesGetKeyValue(hKeyList,
                                     NvOdmKeyListId_ReservedBctCustomerOption);
        NvOdmServicesKeyListClose(hKeyList);
        Personality =
            NV_DRF_VAL(TEGRA_DEVKIT, BCT_CUSTOPT, PERSONALITY, CustomerOption);
	Ril =
            NV_DRF_VAL(TEGRA_DEVKIT, BCT_CUSTOPT, RIL, CustomerOption);
    }

    if (!Personality)
        Personality = TEGRA_DEVKIT_DEFAULT_PERSONALITY;

    if (!Ril)
        Ril = TEGRA_DEVKIT_BCT_CUSTOPT_0_RIL_DEFAULT;

    switch (IoModule)
    {
    case NvOdmIoModule_Display:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Display;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Display);
        break;

    case NvOdmIoModule_Dap:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Dap;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Dap);
        break;

    case NvOdmIoModule_Hdcp:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Hdcp;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Hdcp);
        break;

    case NvOdmIoModule_Hdmi:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Hdmi;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Hdmi);
        break;

    case NvOdmIoModule_I2c:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_I2c;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_I2c);
        break;

    case NvOdmIoModule_I2c_Pmu:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_I2c_Pmu;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_I2c_Pmu);
        break;

    case NvOdmIoModule_Kbd:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Kbd;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Kbd);
        break;

    case NvOdmIoModule_Mio:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Mio;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Mio);
        break;

    case NvOdmIoModule_Nand:
            *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Nand;
            *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Nand);
        break;

    case NvOdmIoModule_Sdio:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Sdio;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Sdio);
        break;

    case NvOdmIoModule_Spdif:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Spdif;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Spdif);
        break;

    case NvOdmIoModule_Spi:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Spi;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Spi);
        break;

    case NvOdmIoModule_Uart:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Uart;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Uart);
        break;

    case NvOdmIoModule_ExternalClock:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_ExternalClock;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_ExternalClock);
        break;

    case NvOdmIoModule_VideoInput:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_VideoInput;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_VideoInput);
        break;

    case NvOdmIoModule_Crt:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Crt;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Crt);
        break;

    case NvOdmIoModule_Tvo:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Tvo;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Tvo);
        break;

    case NvOdmIoModule_Ata:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Ata;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Ata);
        break;

    case NvOdmIoModule_Pwm:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Pwm;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Pwm);
        break;

    case NvOdmIoModule_Dsi:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Dsi;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Dsi);
        break;

    case NvOdmIoModule_Hsi:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Hsi;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Hsi);
        break;

    case NvOdmIoModule_Twc:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Twc;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Twc);
        break;

    case NvOdmIoModule_Ulpi:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Ulpi;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Ulpi);
        break;

    case NvOdmIoModule_OneWire:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_OneWire;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_OneWire);
        break;

    case NvOdmIoModule_SyncNor:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_SyncNor;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_SyncNor);
        break;

    case NvOdmIoModule_PciExpress:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_PciExpress;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_PciExpress);
        break;

    case NvOdmIoModule_Trace:
        if ((Personality == TEGRA_DEVKIT_BCT_CUSTOPT_0_PERSONALITY_11) ||
            (Personality == TEGRA_DEVKIT_BCT_CUSTOPT_0_PERSONALITY_15) ||
            (Personality == TEGRA_DEVKIT_BCT_CUSTOPT_0_PERSONALITY_C1))
        {
            *pPinMuxConfigTable = s_NvOdmPinMuxConfig_Ptm;
            *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_Ptm);
        }
        else
        {
            *pCount = 0;
        }
        break;

    case NvOdmIoModule_BacklightPwm:
        *pPinMuxConfigTable = s_NvOdmPinMuxConfig_BacklightPwm;
        *pCount = NV_ARRAY_SIZE(s_NvOdmPinMuxConfig_BacklightPwm);
        break;

    case NvOdmIoModule_Hsmmc:
    case NvOdmIoModule_Csi:
    case NvOdmIoModule_Sflash:
    case NvOdmIoModule_Slink:
    case NvOdmIoModule_Gpio:
    case NvOdmIoModule_I2s:
    case NvOdmIoModule_Usb:
    case NvOdmIoModule_Vdd:
    case NvOdmIoModule_Xio:
    case NvOdmIoModule_Tsense:
        *pCount = 0;
        break;

    default:
        NV_ASSERT(!"Bad Parameter!");
        *pCount = 0;
        break;
    }
}

void
NvOdmQueryClockLimits(
    NvOdmIoModule IoModule,
    const NvU32 **pClockSpeedLimits,
    NvU32 *pCount)
{
    switch (IoModule)
    {
    case NvOdmIoModule_Hsmmc:
        *pCount = 0;
        break;

    case NvOdmIoModule_Sdio:
        *pClockSpeedLimits = s_NvOdmClockLimit_Sdio;
        *pCount = NVODM_PINMUX_ARRAY_SIZE(s_NvOdmClockLimit_Sdio);
        break;


    default:
        *pClockSpeedLimits = NULL;
        *pCount = 0;
        break;
    }
}

