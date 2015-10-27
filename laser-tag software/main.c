/*
 * Copyright (c) 2015 David Barksdale <amatus@amat.us>
 * Copyright (c) 2013 - 2014, Freescale Semiconductor, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of Freescale Semiconductor, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "fsl_clock_manager.h"
#include "fsl_cmp_driver.h"
#include "fsl_dac_driver.h"
#include "fsl_dma_driver.h"
#include "fsl_gpio_driver.h"
#include "fsl_lptmr_driver.h"
#include "fsl_lpuart_driver.h"
#include "fsl_os_abstraction.h"
#include "fsl_pit_driver.h"
#include "fsl_smc_hal.h"
#include "fsl_spi_master_driver.h"


////////////////////////////
// A bunch of config structs

/* Configuration for enter VLPR mode. Core clock = 2MHz. */
static const clock_manager_user_config_t g_defaultClockConfigVlpr = {   
    .mcgliteConfig =
    {    
        .mcglite_mode       = kMcgliteModeLirc8M,   // Work in LIRC_8M mode.
        .irclkEnable        = true,  // MCGIRCLK enable.
        .irclkEnableInStop  = false, // MCGIRCLK disable in STOP mode.
        .ircs               = kMcgliteLircSel2M, // Select LIRC_2M.
        .fcrdiv             = kMcgliteLircDivBy1,    // FCRDIV is 0.
        .lircDiv2           = kMcgliteLircDivBy1,    // LIRC_DIV2 is 0.
        .hircEnableInNotHircMode         = false, // HIRC disable.
    },
    .simConfig =
    {    
        .er32kSrc  = kClockEr32kSrcOsc0,   // ERCLK32K selection, use OSC.
        .outdiv1   = 0U,
        .outdiv4   = 1U,
    },
    .oscerConfig =
    {   
        .enable       = false, // OSCERCLK disable.
        .enableInStop = false, // OSCERCLK disable in STOP mode.
    }
};

/* Configuration for enter RUN mode. Core clock = 48MHz. */
static const clock_manager_user_config_t g_defaultClockConfigRun = {
    .mcgliteConfig =
    {   
        .mcglite_mode        = kMcgliteModeHirc48M,   // Work in HIRC mode.
        .irclkEnable        = false, // MCGIRCLK disable.
        .irclkEnableInStop  = false, // MCGIRCLK disable in STOP mode.
        .ircs               = kMcgliteLircSel2M, // Select LIRC_2M.
        .fcrdiv             = kMcgliteLircDivBy1,    // FCRDIV is 0.
        .lircDiv2           = kMcgliteLircDivBy1,    // LIRC_DIV2 is 0.
        .hircEnableInNotHircMode         = true,  // HIRC disable.
    },
    .simConfig =
    {   
        .er32kSrc  = kClockEr32kSrcOsc0,  // ERCLK32K selection, use OSC.
        .outdiv1   = 0U,
        .outdiv4   = 1U,
    },
    .oscerConfig =
    {
        .enable       = false, // OSCERCLK disable.
        .enableInStop = false, // OSCERCLK disable in STOP mode.
    }
};

/* Idle the CPU in Very Low Power Wait (VLPW) */
/* This should be the lowest power mode where the PIT still functions. */
static const smc_power_mode_config_t g_idlePowerMode = {
    .powerModeName = kPowerModeVlpw,
};

/* LCD backlight GPIO pin */
static const gpio_output_pin_user_config_t g_lcdBacklight = {
    .pinName = GPIO_MAKE_PIN(GPIOE_IDX, 31),
    .config.outputLogic = 1,
    .config.slewRate = kPortSlowSlewRate,
    .config.driveStrength = kPortLowDriveStrength,
};

/* LCD A0 GPIO pin */
static const gpio_output_pin_user_config_t g_lcdA0 = {
    .pinName = GPIO_MAKE_PIN(GPIOD_IDX, 6),
    .config.outputLogic = 0,
    .config.slewRate = kPortSlowSlewRate,
    .config.driveStrength = kPortLowDriveStrength,
};

/* Switch1 GPIO pin */
static const gpio_input_pin_user_config_t g_switch1 = {
    .pinName = GPIO_MAKE_PIN(GPIOA_IDX, 1),
    .config.isPullEnable = true,
    .config.pullSelect = kPortPullUp,
    .config.interrupt = kPortIntEitherEdge,
};

/* LPTMR configurations */
static const lptmr_user_config_t g_lptmrConfig = {
    .timerMode = kLptmrTimerModeTimeCounter,
    .freeRunningEnable = false,
    .prescalerEnable = true,
    .prescalerClockSource = kClockLptmrSrcLpoClk,
    .prescalerValue = kLptmrPrescalerDivide2,
    .isInterruptEnabled = true,
};

/* PIT config */
static const pit_user_config_t g_pitChan0 = {
    .periodUs = 104, // 9615 Hz (9600 baud 0.16% error)
};

/* CMP config */
static const cmp_comparator_config_t g_cmpConf = {
    .hystersisMode = kCmpHystersisOfLevel0,
    .pinoutEnable = true,
    .pinoutUnfilteredEnable = false,
    .invertEnable = true,
    .highSpeedEnable = false,
    .dmaEnable = false,
    .risingIntEnable = false,
    .fallingIntEnable = false,
    .plusChnMux = kCmpInputChn5,
    .minusChnMux = kCmpInputChnDac,
    .triggerEnable = false,
};

/* CMP DAC config */
static cmp_dac_config_t g_cmpDacConf = {
    .dacEnable = true,
    .refVoltSrcMode = kCmpDacRefVoltSrcOf2,
    .dacValue = 32,
};

/* LPUART0 config */
static lpuart_user_config_t g_lpuartConfig = {
    .clockSource = kClockLpuartSrcMcgIrClk,
    .baudRate = 9600,
    .parityMode = kLpuartParityEven,
    .stopBitCount = kLpuartOneStopBit,
    .bitCountPerChar = kLpuart9BitsPerChar,
};

/* LCD SPI config */
static spi_master_user_config_t g_spi1Config = {
    .bitsPerSec = 100000, // 100 kbps
    .polarity = kSpiClockPolarity_ActiveLow,
    .phase = kSpiClockPhase_SecondEdge,
    .direction = kSpiMsbFirst,
    .bitCount = kSpi8BitMode,
};

///////
// Code

static cmp_state_t g_cmpState;
static dma_channel_t g_chan;
static lpuart_state_t g_lpuartState;
static uint8_t rxBuff[1];
static spi_master_state_t g_spi1State;

/*!
 * @brief LPTMR interrupt call back function.
 * The function is used to toggle LED1.
 */
static void lptmr_call_back(void)
{
    // Toggle LED1
    GPIO_DRV_TogglePinOutput(g_lcdBacklight.pinName);

    // AGC adjust
    if (CMP_DRV_GetOutputLogic(0) != g_cmpConf.invertEnable) {
        if (g_cmpDacConf.dacValue < 63) {
            g_cmpDacConf.dacValue++;
            CMP_DRV_ConfigDacChn(0, &g_cmpDacConf);
        }
    } else {
        if (g_cmpDacConf.dacValue > 0) {
            g_cmpDacConf.dacValue--;
            CMP_DRV_ConfigDacChn(0, &g_cmpDacConf);
        }
    }
}

void PORTA_IRQHandler(void)
{
    /* Clear interrupt flag.*/
    PORT_HAL_ClearPortIntFlag(PORTA_BASE_PTR);

    if (GPIO_DRV_ReadPinInput(g_switch1.pinName)) {
        PIT_DRV_StopTimer(0, 0);
        DAC_DRV_Output(0, 0);
    } else {
        DMA_HAL_SetTransferCount(g_dmaBase[0], g_chan.channel, 0xffff0);
        PIT_DRV_StartTimer(0, 0);
    }
}

static void lpuartRxCallback(uint32_t instance, void *lpuartState)
{
    LPUART_Type *base = g_lpuartBase[instance];
    uint32_t stat = LPUART_RD_STAT(base);
    bool noise = stat & LPUART_STAT_NF_MASK;
    bool frame_error = stat & LPUART_STAT_FE_MASK;
    bool parity_error = stat & LPUART_STAT_PF_MASK;
    LPUART_WR_STAT(base, (stat & 0x3e000000) |
            LPUART_STAT_NF_MASK | LPUART_STAT_FE_MASK | LPUART_STAT_PF_MASK);
}

/*!
 * @brief Main function
 */
int main (void)
{
    /* enable clock for PORTs */
    CLOCK_SYS_EnablePortClock(PORTA_IDX);
    //CLOCK_SYS_EnablePortClock(PORTB_IDX);
    //CLOCK_SYS_EnablePortClock(PORTC_IDX);
    CLOCK_SYS_EnablePortClock(PORTD_IDX);
    CLOCK_SYS_EnablePortClock(PORTE_IDX);

    /* Set allowed power mode, allow all. */
    SMC_HAL_SetProtection(SMC, kAllowPowerModeAll);

    /* Set system clock configuration. */
    CLOCK_SYS_SetConfiguration(&g_defaultClockConfigVlpr);

    /* Initialize LPTMR */
    lptmr_state_t lptmrState;
    LPTMR_DRV_Init(LPTMR0_IDX, &lptmrState, &g_lptmrConfig);
    LPTMR_DRV_SetTimerPeriodUs(LPTMR0_IDX, 100000);
    LPTMR_DRV_InstallCallback(LPTMR0_IDX, lptmr_call_back);

    /* Initialize PIT */
    PIT_DRV_Init(0, false);
    PIT_DRV_InitChannel(0, 0, &g_pitChan0);

    /* Initialize the DAC */
    dac_converter_config_t MyDacUserConfigStruct;
    DAC_DRV_StructInitUserConfigNormal(&MyDacUserConfigStruct);
    DAC_DRV_Init(0, &MyDacUserConfigStruct);

    /* Initialize DMA */
    dma_state_t dma_state;
    DMA_DRV_Init(&dma_state);
    DMA_DRV_RequestChannel(kDmaAnyChannel, kDmaRequestMux0AlwaysOn60, &g_chan);
    DMAMUX_HAL_SetPeriodTriggerCmd(g_dmamuxBase[0], g_chan.channel, true);
    const uint32_t dac_dat = (intptr_t)&DAC_DATL_REG(g_dacBase[0], 0);
    const uint16_t L_ON = 0x4ff;
    const uint16_t L_OFF = 0x2ff;
    const uint16_t laserbuf[16] __attribute__ ((aligned(32))) = {
        L_ON,  L_ON,  L_ON,  L_OFF,
        L_ON,  L_ON,  L_OFF, L_ON,
        L_ON,  L_ON,  L_OFF, L_OFF,
        L_OFF, L_OFF, L_OFF, L_OFF,
    };
    DMA_DRV_ConfigTransfer(&g_chan, kDmaMemoryToPeripheral, 2,
            (intptr_t)laserbuf, dac_dat, 0xffff0);
    DMA_HAL_SetSourceModulo(g_dmaBase[0], g_chan.channel, kDmaModulo32Bytes);
    DMA_HAL_SetAsyncDmaRequestCmd(g_dmaBase[0], g_chan.channel, true);
    DMA_HAL_SetIntCmd(g_dmaBase[0], g_chan.channel, false);
    DMA_HAL_SetDisableRequestAfterDoneCmd(g_dmaBase[0], g_chan.channel, false);
    DMA_DRV_StartChannel(&g_chan);
    DAC_DRV_Output(0, 0);

    /* Initialize CMP */
    CMP_DRV_Init(0, &g_cmpState, &g_cmpConf);
    CMP_DRV_ConfigDacChn(0, &g_cmpDacConf);
    PORT_HAL_SetMuxMode(g_portBase[GPIOE_IDX], 0, kPortMuxAlt5);
    CMP_DRV_Start(0);

    /* Buttons */
    GPIO_DRV_InputPinInit(&g_switch1);

    /* Start LPTMR */
    LPTMR_DRV_Start(LPTMR0_IDX);

    /* Setup LPUART0 */
    LPUART_DRV_Init(1, &g_lpuartState, &g_lpuartConfig);
    LPUART_DRV_InstallRxCallback(1, lpuartRxCallback, rxBuff, NULL, true);
    PORT_HAL_SetMuxMode(g_portBase[GPIOE_IDX], 1, kPortMuxAlt3);

    /* Setup LCD */
    GPIO_DRV_OutputPinInit(&g_lcdBacklight);
    GPIO_DRV_OutputPinInit(&g_lcdA0);
    PORT_HAL_SetMuxMode(g_portBase[GPIOD_IDX], 4, kPortMuxAlt2); // CS
    PORT_HAL_SetMuxMode(g_portBase[GPIOD_IDX], 5, kPortMuxAlt2); // SCL
    PORT_HAL_SetMuxMode(g_portBase[GPIOD_IDX], 7, kPortMuxAlt5); // SI
    SPI_DRV_MasterInit(1, &g_spi1State);
    uint32_t baud;
    SPI_DRV_MasterConfigureBus(1, &g_spi1Config, &baud);
    uint8_t buff[] = {
        0x80, 0x21, // Set electronic "volume" to 33/63
        0x2f, // Set power control: booster on, regulator on, follower on
        0xaf, // Display ON
        0x40, // Set display line start address to 0
        //0xa5, // all points ON
    };
    SPI_DRV_MasterTransferBlocking(1, NULL, buff, NULL, sizeof(buff), 1000);

    // Try to write something to the display
    int i;
    for (i = 0; i < 4; ++i) {
        int j;
        uint8_t buff[] = {
            0xb + i, // Set page address to i
            0x00, 0x10 // Set column address to 0
        };
        GPIO_DRV_ClearPinOutput(g_lcdA0.pinName);
        SPI_DRV_MasterTransferBlocking(1, NULL, buff, NULL, sizeof(buff), 1000);
        GPIO_DRV_SetPinOutput(g_lcdA0.pinName);
        for (j = 0; j < 128; ++j) {
            uint8_t byte = 0x33;
            SPI_DRV_MasterTransferBlocking(1, NULL, &byte, NULL, 1, 1000);
        }
    }

    /* We're done, everything else is triggered through interrupts */
    for(;;) {
#ifndef DEBUG
        SMC_HAL_SetMode(SMC, &g_idlePowerMode);
#endif
    }
}

/* vim: set expandtab ts=4 sw=4: */
