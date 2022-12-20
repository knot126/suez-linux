/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#ifndef __MFD_MT6359_CORE_H__
#define __MFD_MT6359_CORE_H__

enum mt6359_irq_top_status_shift {
	MT6359_BUCK_TOP = 0,
	MT6359_LDO_TOP,
	MT6359_PSC_TOP,
	MT6359_SCK_TOP,
	MT6359_BM_TOP,
	MT6359_HK_TOP,
	MT6359_AUD_TOP = 7,
	MT6359_MISC_TOP,
};

enum mt6359_irq_numbers {
	MT6359_IRQ_VCORE_OC = 1,
	MT6359_IRQ_VGPU11_OC,
	MT6359_IRQ_VGPU12_OC,
	MT6359_IRQ_VMODEM_OC,
	MT6359_IRQ_VPROC1_OC,
	MT6359_IRQ_VPROC2_OC,
	MT6359_IRQ_VS1_OC,
	MT6359_IRQ_VS2_OC,
	MT6359_IRQ_VPA_OC = 9,
	MT6359_IRQ_VFE28_OC = 16,
	MT6359_IRQ_VXO22_OC,
	MT6359_IRQ_VRF18_OC,
	MT6359_IRQ_VRF12_OC,
	MT6359_IRQ_VEFUSE_OC,
	MT6359_IRQ_VCN33_1_OC,
	MT6359_IRQ_VCN33_2_OC,
	MT6359_IRQ_VCN13_OC,
	MT6359_IRQ_VCN18_OC,
	MT6359_IRQ_VA09_OC,
	MT6359_IRQ_VCAMIO_OC,
	MT6359_IRQ_VA12_OC,
	MT6359_IRQ_VAUX18_OC,
	MT6359_IRQ_VAUD18_OC,
	MT6359_IRQ_VIO18_OC,
	MT6359_IRQ_VSRAM_PROC1_OC,
	MT6359_IRQ_VSRAM_PROC2_OC,
	MT6359_IRQ_VSRAM_OTHERS_OC,
	MT6359_IRQ_VSRAM_MD_OC,
	MT6359_IRQ_VEMC_OC,
	MT6359_IRQ_VSIM1_OC,
	MT6359_IRQ_VSIM2_OC,
	MT6359_IRQ_VUSB_OC,
	MT6359_IRQ_VRFCK_OC,
	MT6359_IRQ_VBBCK_OC,
	MT6359_IRQ_VBIF28_OC,
	MT6359_IRQ_VIBR_OC,
	MT6359_IRQ_VIO28_OC,
	MT6359_IRQ_VM18_OC,
	MT6359_IRQ_VUFS_OC = 45,
	MT6359_IRQ_PWRKEY = 48,
	MT6359_IRQ_HOMEKEY,
	MT6359_IRQ_PWRKEY_R,
	MT6359_IRQ_HOMEKEY_R,
	MT6359_IRQ_NI_LBAT_INT,
	MT6359_IRQ_CHRDET_EDGE = 53,
	MT6359_IRQ_RTC = 64,
	MT6359_IRQ_FG_BAT_H = 80,
	MT6359_IRQ_FG_BAT_L,
	MT6359_IRQ_FG_CUR_H,
	MT6359_IRQ_FG_CUR_L,
	MT6359_IRQ_FG_ZCV = 84,
	MT6359_IRQ_FG_N_CHARGE_L = 87,
	MT6359_IRQ_FG_IAVG_H,
	MT6359_IRQ_FG_IAVG_L = 89,
	MT6359_IRQ_FG_DISCHARGE = 91,
	MT6359_IRQ_FG_CHARGE,
	MT6359_IRQ_BATON_LV = 96,
	MT6359_IRQ_BATON_BAT_IN = 98,
	MT6359_IRQ_BATON_BAT_OU,
	MT6359_IRQ_BIF = 100,
	MT6359_IRQ_BAT_H = 112,
	MT6359_IRQ_BAT_L,
	MT6359_IRQ_BAT2_H,
	MT6359_IRQ_BAT2_L,
	MT6359_IRQ_BAT_TEMP_H,
	MT6359_IRQ_BAT_TEMP_L,
	MT6359_IRQ_THR_H,
	MT6359_IRQ_THR_L,
	MT6359_IRQ_AUXADC_IMP,
	MT6359_IRQ_NAG_C_DLTV = 121,
	MT6359_IRQ_AUDIO = 128,
	MT6359_IRQ_ACCDET = 133,
	MT6359_IRQ_ACCDET_EINT0,
	MT6359_IRQ_ACCDET_EINT1,
	MT6359_IRQ_SPI_CMD_ALERT = 144,
	MT6359_IRQ_NR,
};

#define MT6359_IRQ_BUCK_BASE MT6359_IRQ_VCORE_OC
#define MT6359_IRQ_LDO_BASE MT6359_IRQ_VFE28_OC
#define MT6359_IRQ_PSC_BASE MT6359_IRQ_PWRKEY
#define MT6359_IRQ_SCK_BASE MT6359_IRQ_RTC
#define MT6359_IRQ_BM_BASE MT6359_IRQ_FG_BAT_H
#define MT6359_IRQ_HK_BASE MT6359_IRQ_BAT_H
#define MT6359_IRQ_AUD_BASE MT6359_IRQ_AUDIO
#define MT6359_IRQ_MISC_BASE MT6359_IRQ_SPI_CMD_ALERT

#define MT6359_IRQ_BUCK_BITS (MT6359_IRQ_VPA_OC - MT6359_IRQ_BUCK_BASE + 1)
#define MT6359_IRQ_LDO_BITS (MT6359_IRQ_VUFS_OC - MT6359_IRQ_LDO_BASE + 1)
#define MT6359_IRQ_PSC_BITS	\
	(MT6359_IRQ_CHRDET_EDGE - MT6359_IRQ_PSC_BASE + 1)
#define MT6359_IRQ_SCK_BITS (MT6359_IRQ_RTC - MT6359_IRQ_SCK_BASE + 1)
#define MT6359_IRQ_BM_BITS (MT6359_IRQ_BIF - MT6359_IRQ_BM_BASE + 1)
#define MT6359_IRQ_HK_BITS (MT6359_IRQ_NAG_C_DLTV - MT6359_IRQ_HK_BASE + 1)
#define MT6359_IRQ_AUD_BITS	\
	(MT6359_IRQ_ACCDET_EINT1 - MT6359_IRQ_AUD_BASE + 1)
#define MT6359_IRQ_MISC_BITS	\
	(MT6359_IRQ_SPI_CMD_ALERT - MT6359_IRQ_MISC_BASE + 1)

#define MT6359_TOP_GEN(sp)	\
{	\
	.hwirq_base = MT6359_IRQ_##sp##_BASE,	\
	.num_int_regs =	\
		((MT6359_IRQ_##sp##_BITS - 1) /	\
		MTK_PMIC_REG_WIDTH) + 1,	\
	.en_reg = MT6359_##sp##_TOP_INT_CON0,	\
	.en_reg_shift = 0x6,	\
	.sta_reg = MT6359_##sp##_TOP_INT_STATUS0,	\
	.sta_reg_shift = 0x2,	\
	.top_offset = MT6359_##sp##_TOP,	\
}

#endif /* __MFD_MT6359_CORE_H__ */