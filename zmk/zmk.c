/*
 * Copyright 2017 NXP
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

 /**
	This is a ZMK programming example made by www.nxp.com

	ZMK programming guidelines

	A. General initialization guideliness:
	======================================
	1. Transition SSM (System Security Monitor - see SNVS_HPSR[SSM_ST]) from check state to functional state (trusted/secure/non-secure)
	2. Set the correct value in the Power Glitch Detector Register
	3. Clear the power glitch record in the LP Status Register
	4. User Specific: Enable security violations and interrupts in SNVS control and configuration registers		-- see B section
	5. User specific: Program SNVS general functions/configurations		-- see B section
	6. User specific: Set lock bits			-- see B section

	B. To program ZMK by software, do the following:
	================================================
		1. Verify that  ZMK_HWP bit is not set - using SNVS_LPMKCR[ZMK_HWP]
		2. Verify that ZMK is not locked for write
		3. Write key value to the ZMK registers.
		4. Verify that the correct key value is written.
		5. Set ZMK_VAL bit if the ZMK (or the ZMK XORed with the OTPMK) will be used by CAAM as the master key.
			There is no need to set this bit if the ZMK registers are only read by software.
		6. (optional) Set ZMK_ECC_EN bit to enable ZMK error correction code verification.
			Software can verify that the correct nine bit codeword is generated by reading ZMK_ECC_VALUE field.
		7. (optional) Block software read accesses to the ZMK registers and ZMK_ECC_VALUE field by setting ZMK Read lock bit.
		8. (optional) Block software write accesses to the ZMK registers by setting ZMK Write Lock bit.
		9. Set MASTER_KEY_SEL and MKS_EN bits to select combination of OTPMK and ZMK to be provided to the hardware cryptographic module.
		10. (optional) Block software write accesses to the MASTER_KEY_SEL field by setting MKS lock bit.

	NB: Before programming, please note:
	====================================
		i. Please don't consider the SNVS_HPSR[SYS_SECURITY_CFG] field, this should be reserved as it doesn't tie with SECURE_CONFIG fuses
		ii. For closed devices, please add Unlock SNVS ZMK WRITE command in the CSF (follow HABCST_UG userguide ). In this mode ZMK write should be enabled in Closed configuration.
		iii. The ZMK value is zeroized in case of security violation

*/

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/time.h>

#define ADDR_SIZE 4

#define	POR				0x1000
#define	SYSTEM_RESET			0x2000
#define RESET				POR

//SNVS = Secure Non-Volatile Storage registers
#define SNVS_BASE_REG			0x020cc000

//SNVS registers offsets (all these need to be referred using SNVS_BASE_REG)
#define SNVS_HPLR			0x0		//SNVS_HP Lock Register (contains lock bits for the SNVS registers; this is a privileged write register)
	#define ZMK_WSL_MASK		0x1
	#define ZMK_WSL_OFFSET		0
	#define ZMK_RSL_MASK		0x2
	#define ZMK_RSL_OFFSET		1
	#define MKS_SL_MASK		0x0200
	#define MKS_SL_OFFSET		9

#define SNVS_HPCOMR			0x4		//SNVS_HP Command Register
	#define MKS_EN_MASK		0x00002000
	#define MKS_EN_OFFSET		13

#define SNVS_HPCR			0x8		//SNVS_HP Control Register

#define SNVS_HPSR			0x14		//SNVS_HP Status Register	(reflects the internal state of the SNVS)
	#define SSM_ST_MASK		0x00000F00
	#define SSM_ST_OFFSET		8

#define SNVS_LPLR			0x34		//SNVS_LP Lock Register (contains lock bits for the SNVS_LP registers)
	#define ZMK_WHL_MASK		0x1
	#define ZMK_WHL_OFFSET		0
	#define ZMK_RHL_MASK		0x2
	#define ZMK_RHL_OFFSET		1
	#define MKS_HL_MASK		0x200
	#define MKS_HL_OFFSET		9


#define SNVS_LPMKCR			0x3C		//SNVS_LP Master Key Control Register
	#define ZMK_HWP_MASK		0x4
	#define ZMK_HWP_OFFSET		2
	#define ZMK_VAL_MASK		0x8
	#define ZMK_VAL_OFFSET		3
	#define MASTER_KEY_SEL_VALUE	0x2
	#define ZMK_ECC_EN		0x10

#define SNVS_LPSR			0x4c		//SNVS_LP Status Register (reflects the internal state and behavior of the SNVS_LP) (need to write 1 to PGD)
	#define	PGD_MASK		0x8

#define SNVS_LPPGDR			0x64		//SNVS_LP Power Glitch Detector Register (by default need to write 0x41736166 accordint with Security RM)
	#define POWER_GLITCH_VALUE	0x41736166

#define SNVS_LPZMKRn			0x6c		//8 registers SNVS_LPZMKR0 ... SNVS_LPZMKR7; this example will set up only SNVS_LPZMKR0
	#define ZMK_VALUE		0x11223344

#define SNVS_HPVIDR1			0xBF8
	#define IP_ID_MASK		0xFFFF0000
	#define IP_ID_OFFSET		16
	#define MAJOR_REV_MASK		0x0000FF00
	#define MAJOR_REV_OFFSET	8
	#define MINOR_REV_MASK		0x000000FF
	#define MINOR_REV_OFFSET	0

#define SNVS_HPVIDR2			0xBFC

#define get_value_of_SNVS_reg_field(virt_addr, add_offset, field, offset)  ((((*(int*)(((void*)virt_addr)+add_offset))) & field) >> offset)
#define get_SNVS_reg(virt_addr, add_offset)  (int*)(((void*)virt_addr)+add_offset)
#define set_value_of_SNVS_reg(virt_addr, add_offset, value)	*get_SNVS_reg(virt_addr, add_offset) = ((*get_SNVS_reg(virt_addr, add_offset) | (unsigned int)value))


int main(){
	printf("\n\t ZMK Programming Example\n\n");

	int fd = open("/dev/mem", O_SYNC | O_RDWR);
	if (fd < 0) {
		perror ("Can't open /dev/mem!\n");
		return -1;
	}

	unsigned int *mem = mmap (NULL, ADDR_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SNVS_BASE_REG);
	if (mem == MAP_FAILED) {
		perror ("Can't map memory, maybe the address is not truncated\n");
		return -1;
	}

	unsigned int rev1 = *get_SNVS_reg(mem, SNVS_HPVIDR1);
	unsigned int rev2 = *get_SNVS_reg(mem, SNVS_HPVIDR2);

	printf("[INFO] \t SNVS_HPVIDR1=0x%x, SNVS_HPVIDR2=0x%x\n", rev1, rev2);
	printf("[INFO] \t\t  SNVS_HPVIDR1[IP_ID,MAJOR_REV,MINOR_REV]=[0x%x, 0x%x, 0x%x]\n",
		get_value_of_SNVS_reg_field(mem, SNVS_HPVIDR1, IP_ID_MASK, IP_ID_OFFSET),
		get_value_of_SNVS_reg_field(mem, SNVS_HPVIDR1, MAJOR_REV_MASK, MAJOR_REV_OFFSET),
		get_value_of_SNVS_reg_field(mem, SNVS_HPVIDR1, MINOR_REV_MASK, MINOR_REV_OFFSET));

	printf("[INFO] \t The current ZMK key value before starting the ZMK algorithm is 0x%x \n", *get_SNVS_reg(mem, SNVS_LPZMKRn));
	printf("[INFO] \t SNVS_HPLR  = 0x%x\n", *get_SNVS_reg(mem, SNVS_HPLR));
	printf("[INFO] \t SNVS_LPLR  = 0x%x\n", *get_SNVS_reg(mem, SNVS_LPLR));

	//A.1. Check transition SSM (System Security Monitor - see SNVS_HPSR[SSM_ST]) from check state to functional state (trusted/secure/non-secure)
	printf("[INFO] \t A.1. Checking transition SSM (System Security Monitor - see SNVS_HPSR[SSM_ST]) from check state to functional state (trusted/secure/non-secure)\n");
	unsigned char SSM_state = get_value_of_SNVS_reg_field(mem, SNVS_HPSR, SSM_ST_MASK, SSM_ST_OFFSET);
	if ( SSM_state >= 0xB )
	{
		switch (SSM_state) {
			case 0xb:
				printf ("[INFO] \t\t System Security Monitor is in Non-Secure mode\n");
				break;
			case 0xd:
				printf ("[INFO] \t\t System Security Monitor is in Trusted mode\n");
				break;
			case 0xf:
				printf ("[INFO] \t\t System Security Monitor is in Secure mode\n");
				break;
			default:
				printf ("[ERROR] \t\t System Security Monitor is in an undefined mode. Possible to have a hw problem or a Secure-boot issue (check HAB events.\n");
				return -1;
		}

		//A.2. Set the correct value in the Power Glitch Detector Register
		printf("[INFo] \t A.2. Set the correct value in the Power Glitch Detector Register.\n");
		printf("[INFO] \t\t SNVS_LPPGDR power glitch before init 0x%x\n", *get_SNVS_reg(mem, SNVS_LPPGDR));
		set_value_of_SNVS_reg(mem, SNVS_LPPGDR, POWER_GLITCH_VALUE);
		printf("[INFO] \t\t SNVS_LPPGDR power glitch after init 0x%x\n", *get_SNVS_reg(mem, SNVS_LPPGDR));

		//A.3. Clear the power glitch record in the LP Status Register
		printf("[INFO] \t A.3. Clear the power glitch record in the LP Status Register.\n");
		printf("[INFO] \t\t SNVS_LPSR  before init 0x%x\n", *get_SNVS_reg(mem, SNVS_LPSR));
		set_value_of_SNVS_reg(mem, SNVS_LPSR, PGD_MASK);
		printf("[INFO] \t\t SNVS_LPSR  after init 0x%x\n", *get_SNVS_reg(mem, SNVS_LPSR));

		//B.1. Verify that  ZMK_HWP bit is not set - using SNVS_LPMKCR[ZMK_HWP]
		printf("[INFO] \t B.1. Verify that ZMK_HWP bit is not set - using SNVS_LPMKCR[ZMK_HWP]\n");
		printf("[INFO] \t\t SNVS_LPMKCR before check ZMK_HWP 0x%x\n", *get_SNVS_reg(mem, SNVS_LPMKCR));
		unsigned char ZMK_HWP_state = get_value_of_SNVS_reg_field(mem, SNVS_LPMKCR, ZMK_HWP_MASK, ZMK_HWP_OFFSET);
		if ( ZMK_HWP_state == 0x0 )
		{
			printf("[INFO] \t\t SNVS_LPMKCR[ZMK_HWP] Zeroizable Master Key hardware Programming mode is not set.\n");

			//B.2. Verify that ZMK is not locked for write - using SNVS_HPLR, SNVS_LPLR registers
			printf("[INFO] \t B.2. Verify that ZMK is not locked for write - using SNVS_HPLR, SNVS_LPLR registers\n");

			printf("[INFO] \t\t SNVS_HPLR  before checking is 0x%x\n", *get_SNVS_reg(mem, SNVS_HPLR));
			printf("[INFO] \t\t SNVS_LPLR  before checking is 0x%x\n", *get_SNVS_reg(mem, SNVS_LPLR));

			unsigned char ZMK_WSL_state = get_value_of_SNVS_reg_field(mem, SNVS_HPLR, ZMK_WSL_MASK, ZMK_WSL_OFFSET);
			unsigned char ZMK_RSL_state = get_value_of_SNVS_reg_field(mem, SNVS_HPLR, ZMK_RSL_MASK, ZMK_RSL_OFFSET);
			unsigned char MKS_SL_state = get_value_of_SNVS_reg_field(mem, SNVS_HPLR, MKS_SL_MASK, MKS_SL_OFFSET);

			unsigned char MKS_HL_state = get_value_of_SNVS_reg_field(mem, SNVS_LPLR, MKS_HL_MASK, MKS_HL_OFFSET);
			unsigned char ZMK_RHL_state = get_value_of_SNVS_reg_field(mem, SNVS_LPLR, ZMK_RHL_MASK, ZMK_RHL_OFFSET);
			unsigned char ZMK_WHL_state = get_value_of_SNVS_reg_field(mem, SNVS_LPLR, ZMK_WHL_MASK, ZMK_WHL_OFFSET);

			if ((ZMK_WSL_state == 0x0) && (ZMK_RSL_state == 0x0) && (MKS_SL_state == 0x0)) {

				printf("[INFO] \t\t SNVS_HPLR[ZMK_WSL,ZMK_RSL,MKS_SL] Zeroizable Master Write, Read, Select Soft Locks fields are not set.\n");

				if ((ZMK_WHL_state == 0x0) && (ZMK_RHL_state == 0x0) && (MKS_HL_state == 0x0)) {
					printf("[INFO] \t\t SNVS_LPLR[ZMK_WHL,ZMK_RHL,MKS_HL] Zeroizable Master Write, Read, Select Hard Locks fields are not set.\n");

					printf("[INFO] \t\t SNVS_LPLR[MKS_HL] Master Key Select Hard Lock is not set.\n");

					printf("[INFO] \t\t SNVS_LPLR[ZMK_RHL] Zeroizable Master Key Read Hard Lock is not set.\n");

					printf("[INFO] \t B.3. Write key value to the ZMK registers.\n");

					printf("[INFO] \t\t The ZMK key value before writing with 0x%x is 0x%x \n", ZMK_VALUE, *get_SNVS_reg(mem, SNVS_LPZMKRn));

					set_value_of_SNVS_reg(mem, SNVS_LPZMKRn, ZMK_VALUE);

					printf("[INFO] \t B.4. Verify that the correct key value is written.\n");
					if (*get_SNVS_reg(mem, SNVS_LPZMKRn) == ZMK_VALUE) {
						printf("[SUCCESS] \t\t The new ZMK key value is = 0x%x and matches with the user desired value.\n", *get_SNVS_reg(mem, SNVS_LPZMKRn));

						printf("[INFO] \t B.5. Set SNVS_LPMKCR[ZMK_VAL] bit if the ZMK (or the ZMK XORed with the OTPMK) will be used by CAAM as the master key.\n");

						printf("[INFO] \t\t SNVS_LPMKCR  before init 0x%x\n", *get_SNVS_reg(mem, SNVS_LPMKCR));
						set_value_of_SNVS_reg(mem, SNVS_LPMKCR, ZMK_VAL_MASK);
						printf("[INFO] \t\t SNVS_LPMKCR  after init 0x%x\n", *get_SNVS_reg(mem, SNVS_LPMKCR));

						printf("[INFO] \t B.6 (optional) Set SNVS_LPMKCR[ZMK_ECC_EN] bit to enable ZMK error correction code verification. \
								\n\t Software can verify that the correct nine bit codeword is generated by reading ZMK_ECC_VALUE field.\n");
						set_value_of_SNVS_reg(mem, SNVS_LPMKCR, ZMK_ECC_EN);

						printf("[INFO] \t B.7 (optional) Block software read accesses to the ZMK registers and ZMK_ECC_VALUE field by setting ZMK Read lock bit.\n");
						printf("[INFO] \t B.8 (optional) Block software write accesses to the ZMK registers by setting ZMK Write Lock bit.\n");

						if (RESET == POR) {
							//POR to clear next bits
							set_value_of_SNVS_reg(mem, SNVS_LPLR, ZMK_RHL_MASK);
							set_value_of_SNVS_reg(mem, SNVS_LPLR, ZMK_WHL_MASK);
						}
						else {
							//system reset to clear next bits
							set_value_of_SNVS_reg(mem, SNVS_HPLR, ZMK_RSL_MASK);
							set_value_of_SNVS_reg(mem, SNVS_HPLR, ZMK_WSL_MASK);
						}

						//Let some time for SNVS_LPZMKRn to be cleared after ZMK_RHL was set
						#define TIMEOUT_MAX_VAL 0x1000
						volatile int i = 0;
						for (i=0; i<TIMEOUT_MAX_VAL; i++){}

						printf("[INFO] \t\t [SECURITY_CHECK] if SNVS_LPZMKRn is zero'd after ZMK_RHL was set\n");
						if (*get_SNVS_reg(mem, SNVS_LPZMKRn) == 0x0) {
							printf("[INFO] \t\t [PASSED] - SNVS_LPZMKRn is 0x0 and cannot be read by a hacker\n");
						}
						else {
							printf("[INFO] \t\t [FAILED] - SNVS_LPZMKRn is 0x%x and can be read by a hacker. Try to increase the TIMEOUT_MAX_VAL\n", *get_SNVS_reg(mem, SNVS_LPZMKRn));
						}

						printf("[INFO] \t B.9. Set SNVS_LPMKCR[MASTER_KEY_SEL] and SNVS_HPCOMR[MKS_EN] bits to select combination of OTPMK and ZMK to be provided to the hardware cryptographic module.\n");
						printf("[INFO] \t\t For our example MASTER_KEY_SEL is set as 0b10 - Select zeroizable master key when MKS_EN bit is set.\n");
						set_value_of_SNVS_reg(mem, SNVS_LPMKCR, MASTER_KEY_SEL_VALUE);
						printf("[INFO] \t\t SNVS_LPMKCR  after init 0x%x\n", *get_SNVS_reg(mem, SNVS_LPMKCR));

						printf("[INFO] \t\t SNVS_HPCOMR  before init 0x%x\n", *get_SNVS_reg(mem, SNVS_HPCOMR));
						set_value_of_SNVS_reg(mem, SNVS_HPCOMR, MKS_EN_MASK);
						printf("[INFO] \t\t SNVS_HPCOMR  after init 0x%x\n", *get_SNVS_reg(mem, SNVS_HPCOMR));

						printf("[INFO] \t B.10 (optional) Block software write accesses to the MASTER_KEY_SEL field by setting MKS lock bit.\n");

						if (RESET == POR) {
							//POR to clear next bit
							set_value_of_SNVS_reg(mem, SNVS_LPLR, MKS_HL_MASK);
						}
						else {
							//system reset to clear next bit
							set_value_of_SNVS_reg(mem, SNVS_HPLR, MKS_SL_MASK);
						}
					}
					else {
						printf("[ERROR] \t\t The new ZMK key value 0x%x is not matching with the user desire value!!! \n", *get_SNVS_reg(mem, SNVS_LPZMKRn));
					}
				}
				else {
					printf("[ERROR] \t\t SNVS_LPLR[ZMK_WHL,ZMK_RHL,MKS_HL] Zeroizable Master Write, Read, Select Hard Locks one of these bits are set - Write access is not allowed.\
					Once set, these bits can only be cleared by the LP LOR. \n");
					return -1;
				}
			}
			else {
				printf("[ERROR] \t\t SNVS_HPLR[ZMK_WSL,ZMK_RSL,MKS_SL] Zeroizable Master Write, Read, Select Soft Locks one of these bits are set - Write access is not allowed.\
				Once set, these bits can only be cleared by system reset. \n");
				return -1;
			}
		}
		else {
			printf("[ERROR] \t\t  SNVS_LPMKCR[ZMK_HWP] Zeroizable Master Key hardware Programming mode is set.  \
			ZMK is in the hardware programming mode, cannot be programmed by software. See the ZMK hardware programming mechanism in Security RM.\n");
			return -1;
		}
	}
	else {
		printf("[ERROR] \t\t Transition of SSM[System Security Monitor] is not trusted, secure or non-secure. Please check the Security Reference Manual for more details.\n");
		return -1;
	}

	return 0;
}
