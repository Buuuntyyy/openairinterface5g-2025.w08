/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/* \file rrc_UE.c
 * \brief RRC procedures
 * \author R. Knopp, K.H. HSU
 * \date 2018
 * \version 0.1
 * \company Eurecom / NTUST
 * \email: knopp@eurecom.fr, kai-hsiang.hsu@eurecom.fr
 * \note
 * \warning
 */

#define RRC_UE
#define RRC_UE_C

#include "NR_DL-DCCH-Message.h"        //asn_DEF_NR_DL_DCCH_Message
#include "NR_DL-CCCH-Message.h"        //asn_DEF_NR_DL_CCCH_Message
#include "NR_BCCH-BCH-Message.h"       //asn_DEF_NR_BCCH_BCH_Message
#include "NR_BCCH-DL-SCH-Message.h"    //asn_DEF_NR_BCCH_DL_SCH_Message
#include "NR_CellGroupConfig.h"        //asn_DEF_NR_CellGroupConfig
#include "NR_BWP-Downlink.h"           //asn_DEF_NR_BWP_Downlink
#include "NR_RRCReconfiguration.h"
#include "NR_MeasConfig.h"
#include "NR_UL-DCCH-Message.h"
#include "uper_encoder.h"
#include "uper_decoder.h"

#include "rrc_defs.h"
#include "rrc_proto.h"
#include "openair2/RRC/LTE/rrc_defs.h"
#include "L2_interface_ue.h"
#include "LAYER2/NR_MAC_UE/mac_proto.h"

#include "intertask_interface.h"

#include "LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "nr-uesoftmodem.h"
#include "plmn_data.h"
#include "nr_pdcp/nr_pdcp_oai_api.h"
#include "openair3/SECU/secu_defs.h"
#include "openair3/SECU/key_nas_deriver.h"

#include "common/utils/LOG/log.h"
#include "common/utils/LOG/vcd_signal_dumper.h"

#ifndef CELLULAR
  #include "RRC/NR/MESSAGES/asn1_msg.h"
#endif

#include "SIMULATION/TOOLS/sim.h" // for taus

#include "nr_nas_msg.h"
#include "openair2/SDAP/nr_sdap/nr_sdap_entity.h"

static NR_UE_RRC_INST_t *NR_UE_rrc_inst;
/* NAS Attach request with IMSI */
static const char nr_nas_attach_req_imsi_dummy_NSA_case[] = {
    0x07,
    0x41,
    /* EPS Mobile identity = IMSI */
    0x71,
    0x08,
    0x29,
    0x80,
    0x43,
    0x21,
    0x43,
    0x65,
    0x87,
    0xF9,
    /* End of EPS Mobile Identity */
    0x02,
    0xE0,
    0xE0,
    0x00,
    0x20,
    0x02,
    0x03,
    0xD0,
    0x11,
    0x27,
    0x1A,
    0x80,
    0x80,
    0x21,
    0x10,
    0x01,
    0x00,
    0x00,
    0x10,
    0x81,
    0x06,
    0x00,
    0x00,
    0x00,
    0x00,
    0x83,
    0x06,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x0D,
    0x00,
    0x00,
    0x0A,
    0x00,
    0x52,
    0x12,
    0xF2,
    0x01,
    0x27,
    0x11,
};

static void nr_rrc_manage_rlc_bearers(NR_UE_RRC_INST_t *rrc,
                                      const NR_CellGroupConfig_t *cellGroupConfig);

static void nr_rrc_ue_process_RadioBearerConfig(NR_UE_RRC_INST_t *ue_rrc,
                                                NR_RadioBearerConfig_t *const radioBearerConfig);
static void nr_rrc_ue_generate_rrcReestablishmentComplete(const NR_UE_RRC_INST_t *rrc, const NR_RRCReestablishment_t *rrcReestablishment);
static void process_lte_nsa_msg(NR_UE_RRC_INST_t *rrc, nsa_msg_t *msg, int msg_len);
static void nr_rrc_ue_process_ueCapabilityEnquiry(NR_UE_RRC_INST_t *rrc, NR_UECapabilityEnquiry_t *UECapabilityEnquiry);
static void nr_rrc_ue_process_masterCellGroup(NR_UE_RRC_INST_t *rrc,
                                              OCTET_STRING_t *masterCellGroup,
                                              long *fullConfig);

static void nr_rrc_ue_process_measConfig(rrcPerNB_t *rrc, NR_MeasConfig_t *const measConfig, NR_UE_Timers_Constants_t *timers);

NR_UE_RRC_INST_t* get_NR_UE_rrc_inst(int instance)
{
  return &NR_UE_rrc_inst[instance];
}

static NR_RB_status_t get_DRB_status(const NR_UE_RRC_INST_t *rrc, NR_DRB_Identity_t drb_id)
{
  AssertFatal(drb_id > 0 && drb_id < 33, "Invalid DRB ID %ld\n", drb_id);
  return rrc->status_DRBs[drb_id - 1];
}

static void set_DRB_status(NR_UE_RRC_INST_t *rrc, NR_DRB_Identity_t drb_id, NR_RB_status_t status)
{
  AssertFatal(drb_id > 0 && drb_id < 33, "Invalid DRB ID %ld\n", drb_id);
  rrc->status_DRBs[drb_id - 1] = status;
}

static void nr_decode_SI(NR_UE_RRC_SI_INFO *SI_info, NR_SystemInformation_t *si, NR_UE_RRC_INST_t *rrc)
{
  instance_t ue_id = rrc->ue_id;
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_RRC_UE_DECODE_SI, VCD_FUNCTION_IN);

  // Dump contents
  if (si->criticalExtensions.present == NR_SystemInformation__criticalExtensions_PR_systemInformation
      || si->criticalExtensions.present == NR_SystemInformation__criticalExtensions_PR_criticalExtensionsFuture_r16) {
    LOG_D(NR_RRC,
          "[UE] si->criticalExtensions.choice.NR_SystemInformation_t->sib_TypeAndInfo.list.count %d\n",
          si->criticalExtensions.choice.systemInformation->sib_TypeAndInfo.list.count);
  } else {
    LOG_D(NR_RRC, "[UE] Unknown criticalExtension version (not Rel16)\n");
    return;
  }

  NR_SIB19_r17_t *sib19 = NULL;
  for (int i = 0; i < si->criticalExtensions.choice.systemInformation->sib_TypeAndInfo.list.count; i++) {
    SystemInformation_IEs__sib_TypeAndInfo__Member *typeandinfo;
    typeandinfo = si->criticalExtensions.choice.systemInformation->sib_TypeAndInfo.list.array[i];
    LOG_I(NR_RRC, "Found SIB%d\n", typeandinfo->present + 1);
    switch(typeandinfo->present) {
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib2:
        SI_info->sib2_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib2_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib3:
        SI_info->sib3_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib3_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib4:
        SI_info->sib4_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib4_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib5:
        SI_info->sib5_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib5_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib6:
        SI_info->sib6_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib6_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib7:
        SI_info->sib7_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib7_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib8:
        SI_info->sib8_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib8_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib9:
        SI_info->sib9_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib9_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib10_v1610:
        SI_info->sib10_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib10_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib11_v1610:
        SI_info->sib11_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib11_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib12_v1610:
        SI_info->sib12_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib12_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib13_v1610:
        SI_info->sib13_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib13_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib14_v1610:
        SI_info->sib14_validity = SIB_VALID;
        nr_timer_start(&SI_info->sib14_timer);
        break;
      case NR_SystemInformation_IEs__sib_TypeAndInfo__Member_PR_sib19_v1700:
        SI_info->SInfo_r17.sib19_validity = SIB_VALID;
        if (g_log->log_component[NR_RRC].level >= OAILOG_DEBUG)
          xer_fprint(stdout, &asn_DEF_NR_SIB19_r17, (const void *)typeandinfo->choice.sib19_v1700);
        sib19 = typeandinfo->choice.sib19_v1700;
        nr_timer_start(&SI_info->SInfo_r17.sib19_timer);
        break;
      default:
        break;
    }
  }

  if (sib19) {
    MessageDef *msg = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_MAC_RRC_CONFIG_OTHER_SIB);
    asn_copy(&asn_DEF_NR_SIB19_r17, (void **)&NR_MAC_RRC_CONFIG_OTHER_SIB(msg).sib19, sib19);
    NR_MAC_RRC_CONFIG_OTHER_SIB(msg).can_start_ra = rrc->is_NTN_UE;
    itti_send_msg_to_task(TASK_MAC_UE, ue_id, msg);
  }
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_RRC_UE_DECODE_SI, VCD_FUNCTION_OUT);
}

static void nr_rrc_ue_prepare_RRCSetupRequest(NR_UE_RRC_INST_t *rrc)
{
  LOG_D(NR_RRC, "Generation of RRCSetupRequest\n");
  uint8_t rv[6];
  // Get RRCConnectionRequest, fill random for now
  // Generate random byte stream for contention resolution
  for (int i = 0; i < 6; i++) {
#ifdef SMBV
    // if SMBV is configured the contention resolution needs to be fix for the connection procedure to succeed
    rv[i] = i;
#else
    rv[i] = taus() & 0xff;
#endif
  }

  uint8_t buf[1024];
  int len = do_RRCSetupRequest(buf, sizeof(buf), rv, rrc->fiveG_S_TMSI);

  nr_rlc_srb_recv_sdu(rrc->ue_id, 0, buf, len);
}

static void nr_rrc_configure_default_SI(NR_UE_RRC_SI_INFO *SI_info,
                                        struct NR_SI_SchedulingInfo *si_SchedulingInfo,
                                        struct NR_SI_SchedulingInfo_v1700 *si_SchedulingInfo_v1700)
{
  if (si_SchedulingInfo) {
    SI_info->default_otherSI_map = 0;
    for (int i = 0; i < si_SchedulingInfo->schedulingInfoList.list.count; i++) {
      struct NR_SchedulingInfo *schedulingInfo = si_SchedulingInfo->schedulingInfoList.list.array[i];
      for (int j = 0; j < schedulingInfo->sib_MappingInfo.list.count; j++) {
        struct NR_SIB_TypeInfo *sib_Type = schedulingInfo->sib_MappingInfo.list.array[j];
        SI_info->default_otherSI_map |= 1 << sib_Type->type;
      }
    }
  }

  if (si_SchedulingInfo_v1700) {
    SI_info->SInfo_r17.default_otherSI_map_r17 = 0;
    for (int i = 0; i < si_SchedulingInfo_v1700->schedulingInfoList2_r17.list.count; i++) {
      struct NR_SchedulingInfo2_r17 *schedulingInfo2 = si_SchedulingInfo_v1700->schedulingInfoList2_r17.list.array[i];
      for (int j = 0; j < schedulingInfo2->sib_MappingInfo_r17.list.count; j++) {
        struct NR_SIB_TypeInfo_v1700 *sib_TypeInfo_v1700 = schedulingInfo2->sib_MappingInfo_r17.list.array[j];
        if (sib_TypeInfo_v1700->sibType_r17.present == NR_SIB_TypeInfo_v1700__sibType_r17_PR_type1_r17) {
          SI_info->SInfo_r17.default_otherSI_map_r17 |= 1 << sib_TypeInfo_v1700->sibType_r17.choice.type1_r17;
        }
      }
    }
  }
}

static bool verify_NTN_access(const NR_UE_RRC_SI_INFO *SI_info, const NR_SIB1_v1700_IEs_t *sib1_v1700)
{
  // SIB1 indicates if NTN access is present in the cell
  bool ntn_access = false;
  if (sib1_v1700 && sib1_v1700->cellBarredNTN_r17
      && *sib1_v1700->cellBarredNTN_r17 == NR_SIB1_v1700_IEs__cellBarredNTN_r17_notBarred)
    ntn_access = true;

  uint32_t sib19_mask = 1 << NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType19;
  int sib19_present = SI_info->SInfo_r17.default_otherSI_map_r17 & sib19_mask;

  AssertFatal(!ntn_access || sib19_present, "NTN cell, but SIB19 not configured.\n");

  return ntn_access && sib19_present;
}

static void nr_rrc_process_sib1(NR_UE_RRC_INST_t *rrc, NR_UE_RRC_SI_INFO *SI_info, NR_SIB1_t *sib1)
{
  if(g_log->log_component[NR_RRC].level >= OAILOG_DEBUG)
    xer_fprint(stdout, &asn_DEF_NR_SIB1, (const void *) sib1);
  LOG_A(NR_RRC, "SIB1 decoded\n");
  nr_timer_start(&SI_info->sib1_timer);
  SI_info->sib1_validity = SIB_VALID;
  if (rrc->nrRrcState == RRC_STATE_IDLE_NR) {
    rrc->ra_trigger = RRC_CONNECTION_SETUP;
    // preparing RRC setup request payload in advance
    nr_rrc_ue_prepare_RRCSetupRequest(rrc);
  }

  NR_SIB1_v1700_IEs_t *sib1_v1700 = NULL;
  NR_SI_SchedulingInfo_v1700_t *si_SchedInfo_v1700 = NULL;
  if (sib1->nonCriticalExtension
      && sib1->nonCriticalExtension->nonCriticalExtension
      && sib1->nonCriticalExtension->nonCriticalExtension->nonCriticalExtension) {
    sib1_v1700 = sib1->nonCriticalExtension->nonCriticalExtension->nonCriticalExtension;
    si_SchedInfo_v1700 = sib1_v1700->si_SchedulingInfo_v1700;
  }

  // configure default SI
  nr_rrc_configure_default_SI(SI_info, sib1->si_SchedulingInfo, si_SchedInfo_v1700);
  rrc->is_NTN_UE = verify_NTN_access(SI_info, sib1_v1700);

  // configure timers and constant
  nr_rrc_set_sib1_timers_and_constants(&rrc->timers_and_constants, sib1);
  // RRC storage of SIB1 timers and constants (eg needed in re-establishment)
  UPDATE_IE(rrc->timers_and_constants.sib1_TimersAndConstants, sib1->ue_TimersAndConstants, NR_UE_TimersAndConstants_t);
  MessageDef *msg = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_MAC_RRC_CONFIG_SIB1);
  NR_MAC_RRC_CONFIG_SIB1(msg).sib1 = sib1;
  NR_MAC_RRC_CONFIG_SIB1(msg).can_start_ra = !rrc->is_NTN_UE;
  itti_send_msg_to_task(TASK_MAC_UE, rrc->ue_id, msg);
}

static void nr_rrc_process_reconfiguration_v1530(NR_UE_RRC_INST_t *rrc, NR_RRCReconfiguration_v1530_IEs_t *rec_1530, int gNB_index)
{
  if (rec_1530->fullConfig) {
    // TODO perform the full configuration procedure as specified in 5.3.5.11 of 331
    LOG_E(NR_RRC, "RRCReconfiguration includes fullConfig but this is not implemented yet\n");
  }
  if (rec_1530->masterCellGroup)
    nr_rrc_ue_process_masterCellGroup(rrc, rec_1530->masterCellGroup, rec_1530->fullConfig);
  if (rec_1530->masterKeyUpdate) {
    // TODO perform AS security key update procedure as specified in 5.3.5.7
    LOG_E(NR_RRC, "RRCReconfiguration includes masterKeyUpdate but this is not implemented yet\n");
  }
  /* Check if there is dedicated NAS information to forward to NAS */
  if (rec_1530->dedicatedNAS_MessageList) {
    struct NR_RRCReconfiguration_v1530_IEs__dedicatedNAS_MessageList *tmp = rec_1530->dedicatedNAS_MessageList;
    for (int i = 0; i < tmp->list.count; i++) {
      MessageDef *ittiMsg = itti_alloc_new_message(TASK_RRC_NRUE, rrc->ue_id, NAS_CONN_ESTABLI_CNF);
      nas_establish_cnf_t *msg = &NAS_CONN_ESTABLI_CNF(ittiMsg);
      msg->errCode = AS_SUCCESS;
      msg->nasMsg.length = tmp->list.array[i]->size;
      msg->nasMsg.nas_data = tmp->list.array[i]->buf;
      itti_send_msg_to_task(TASK_NAS_NRUE, rrc->ue_id, ittiMsg);
    }
    tmp->list.count = 0; // to prevent the automatic free by ASN1_FREE
  }
  NR_UE_RRC_SI_INFO *SI_info = &rrc->perNB[gNB_index].SInfo;
  if (rec_1530->dedicatedSIB1_Delivery) {
    NR_SIB1_t *sib1 = NULL;
    asn_dec_rval_t dec_rval = uper_decode(NULL,
                                          &asn_DEF_NR_SIB1,
                                          (void **)&sib1,
                                          (uint8_t *)rec_1530->dedicatedSIB1_Delivery->buf,
                                          rec_1530->dedicatedSIB1_Delivery->size,
                                          0,
                                          0);
    if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
      LOG_E(NR_RRC, "dedicatedSIB1-Delivery decode error\n");
      SEQUENCE_free(&asn_DEF_NR_SIB1, sib1, 1);
    } else {
      // mac layer will free sib1
      nr_rrc_process_sib1(rrc, SI_info, sib1);
    }
  }
  if (rec_1530->dedicatedSystemInformationDelivery) {
    NR_SystemInformation_t *si = NULL;
    asn_dec_rval_t dec_rval = uper_decode(NULL,
                                          &asn_DEF_NR_SystemInformation,
                                          (void **)&si,
                                          (uint8_t *)rec_1530->dedicatedSystemInformationDelivery->buf,
                                          rec_1530->dedicatedSystemInformationDelivery->size,
                                          0,
                                          0);
    if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
      LOG_E(NR_RRC, "dedicatedSystemInformationDelivery decode error\n");
      SEQUENCE_free(&asn_DEF_NR_SystemInformation, si, 1);
    } else {
      LOG_I(NR_RRC, "[UE %ld] Decoding dedicatedSystemInformationDelivery\n", rrc->ue_id);
      nr_decode_SI(SI_info, si, rrc);
    }
  }
  if (rec_1530->otherConfig) {
    // TODO perform the other configuration procedure as specified in 5.3.5.9
    LOG_E(NR_RRC, "RRCReconfiguration includes otherConfig but this is not handled yet\n");
  }
  NR_RRCReconfiguration_v1540_IEs_t *rec_1540 = rec_1530->nonCriticalExtension;
  if (rec_1540) {
    NR_RRCReconfiguration_v1560_IEs_t *rec_1560 = rec_1540->nonCriticalExtension;
    if (rec_1560->sk_Counter) {
      // TODO perform AS security key update procedure as specified in 5.3.5.7
      LOG_E(NR_RRC, "RRCReconfiguration includes sk-Counter but this is not implemented yet\n");
    }
    if (rec_1560->mrdc_SecondaryCellGroupConfig) {
      // TODO perform handling of mrdc-SecondaryCellGroupConfig as specified in 5.3.5.3
      LOG_E(NR_RRC, "RRCReconfiguration includes mrdc-SecondaryCellGroupConfig but this is not handled yet\n");
    }
    if (rec_1560->radioBearerConfig2) {
      NR_RadioBearerConfig_t *RadioBearerConfig = NULL;
      asn_dec_rval_t dec_rval = uper_decode(NULL,
                                            &asn_DEF_NR_RadioBearerConfig,
                                            (void **)&RadioBearerConfig,
                                            (uint8_t *)rec_1560->radioBearerConfig2->buf,
                                            rec_1560->radioBearerConfig2->size,
                                            0,
                                            0);
      if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
        LOG_E(NR_RRC, "radioBearerConfig2 decode error\n");
        SEQUENCE_free(&asn_DEF_NR_RadioBearerConfig, RadioBearerConfig, 1);
      } else
        nr_rrc_ue_process_RadioBearerConfig(rrc, RadioBearerConfig);
    }
  }
}

static void nr_rrc_ue_process_rrcReconfiguration(NR_UE_RRC_INST_t *rrc, int gNB_index, NR_RRCReconfiguration_t *reconfiguration)
{
  rrcPerNB_t *rrcNB = rrc->perNB + gNB_index;

  switch (reconfiguration->criticalExtensions.present) {
    case NR_RRCReconfiguration__criticalExtensions_PR_rrcReconfiguration: {
      NR_RRCReconfiguration_IEs_t *ie = reconfiguration->criticalExtensions.choice.rrcReconfiguration;

      if (ie->radioBearerConfig) {
        LOG_I(NR_RRC, "RRCReconfiguration includes radio Bearer Configuration\n");
        nr_rrc_ue_process_RadioBearerConfig(rrc, ie->radioBearerConfig);
        if (LOG_DEBUGFLAG(DEBUG_ASN1))
          xer_fprint(stdout, &asn_DEF_NR_RadioBearerConfig, (const void *)ie->radioBearerConfig);
      }

      if (ie->nonCriticalExtension)
        nr_rrc_process_reconfiguration_v1530(rrc, ie->nonCriticalExtension, gNB_index);

      if (ie->secondaryCellGroup) {
        NR_CellGroupConfig_t *cellGroupConfig = NULL;
        asn_dec_rval_t dec_rval = uper_decode(NULL,
                                              &asn_DEF_NR_CellGroupConfig, // might be added prefix later
                                              (void **)&cellGroupConfig,
                                              (uint8_t *)ie->secondaryCellGroup->buf,
                                              ie->secondaryCellGroup->size,
                                              0,
                                              0);
        if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
          uint8_t *buffer = ie->secondaryCellGroup->buf;
          LOG_E(NR_RRC, "NR_CellGroupConfig decode error\n");
          for (int i = 0; i < ie->secondaryCellGroup->size; i++)
            LOG_E(NR_RRC, "%02x ", buffer[i]);
          LOG_E(NR_RRC, "\n");
          // free the memory
          SEQUENCE_free(&asn_DEF_NR_CellGroupConfig, (void *)cellGroupConfig, 1);
        }

        if (LOG_DEBUGFLAG(DEBUG_ASN1))
          xer_fprint(stdout, &asn_DEF_NR_CellGroupConfig, (const void *) cellGroupConfig);

        nr_rrc_cellgroup_configuration(rrc, cellGroupConfig);

        AssertFatal(!IS_SA_MODE(get_softmodem_params()), "secondaryCellGroup only used in NSA for now\n");
        MessageDef *msg = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_MAC_RRC_CONFIG_CG);
        // cellGroupConfig will be managed by MAC
        NR_MAC_RRC_CONFIG_CG(msg).cellGroupConfig = cellGroupConfig;
        // UE_NR_Capability remain a race condition between this rrc thread and mac thread
        NR_MAC_RRC_CONFIG_CG(msg).UE_NR_Capability = rrc->UECap.UE_NR_Capability;
        itti_send_msg_to_task(TASK_MAC_UE, rrc->ue_id, msg);
      }
      if (ie->measConfig) {
        LOG_I(NR_RRC, "RRCReconfiguration includes Measurement Configuration\n");
        nr_rrc_ue_process_measConfig(rrcNB, ie->measConfig, &rrc->timers_and_constants);
      }
      if (ie->lateNonCriticalExtension) {
        LOG_E(NR_RRC, "RRCReconfiguration includes lateNonCriticalExtension. Not handled.\n");
      }
    } break;
    case NR_RRCReconfiguration__criticalExtensions_PR_NOTHING:
    case NR_RRCReconfiguration__criticalExtensions_PR_criticalExtensionsFuture:
    default:
      break;
  }
  return;
}

void process_nsa_message(NR_UE_RRC_INST_t *rrc, nsa_message_t nsa_message_type, void *message, int msg_len)
{
  switch (nsa_message_type) {
    case nr_SecondaryCellGroupConfig_r15: {
      NR_RRCReconfiguration_t *RRCReconfiguration=NULL;
      asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                     &asn_DEF_NR_RRCReconfiguration,
                                                     (void **)&RRCReconfiguration,
                                                     (uint8_t *)message,
                                                     msg_len);
      if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
        LOG_E(NR_RRC, "NR_RRCReconfiguration decode error\n");
        // free the memory
        SEQUENCE_free(&asn_DEF_NR_RRCReconfiguration, RRCReconfiguration, 1);
        return;
      }
      nr_rrc_ue_process_rrcReconfiguration(rrc, 0, RRCReconfiguration);
      ASN_STRUCT_FREE(asn_DEF_NR_RRCReconfiguration, RRCReconfiguration);
    }
    break;

    case nr_RadioBearerConfigX_r15: {
      NR_RadioBearerConfig_t *RadioBearerConfig=NULL;
      asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                     &asn_DEF_NR_RadioBearerConfig,
                                                     (void **)&RadioBearerConfig,
                                                     (uint8_t *)message,
                                                     msg_len);
      if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
        LOG_E(NR_RRC, "NR_RadioBearerConfig decode error\n");
        // free the memory
        SEQUENCE_free( &asn_DEF_NR_RadioBearerConfig, RadioBearerConfig, 1 );
        return;
      }
      LOG_D(NR_RRC, "Calling nr_rrc_ue_process_RadioBearerConfig()with: e_rab_id = %ld, drbID = %ld, cipher_algo = %ld, key = %ld \n",
            RadioBearerConfig->drb_ToAddModList->list.array[0]->cnAssociation->choice.eps_BearerIdentity,
            RadioBearerConfig->drb_ToAddModList->list.array[0]->drb_Identity,
            RadioBearerConfig->securityConfig->securityAlgorithmConfig->cipheringAlgorithm,
            *RadioBearerConfig->securityConfig->keyToUse);
      nr_rrc_ue_process_RadioBearerConfig(rrc, RadioBearerConfig);
      if (LOG_DEBUGFLAG(DEBUG_ASN1))
        xer_fprint(stdout, &asn_DEF_NR_RadioBearerConfig, (const void *)RadioBearerConfig);
      ASN_STRUCT_FREE(asn_DEF_NR_RadioBearerConfig, RadioBearerConfig);
    }
    break;
    
    default:
      AssertFatal(1==0,"Unknown message %d\n",nsa_message_type);
      break;
  }
}

/**
 * @brief Verify UE capabilities parameters against CL-fed params
 *        (e.g. number of physical TX antennas)
 */
static bool verify_ue_cap(NR_UE_NR_Capability_t *UE_NR_Capability, int nb_antennas_tx)
{
  NR_FeatureSetUplink_t *ul_feature_setup = UE_NR_Capability->featureSets->featureSetsUplink->list.array[0];
  int srs_ant_ports = 1 << ul_feature_setup->supportedSRS_Resources->maxNumberSRS_Ports_PerResource;
  AssertFatal(srs_ant_ports <= nb_antennas_tx, "SRS antenna ports (%d) > nb_antennas_tx (%d)\n", srs_ant_ports, nb_antennas_tx);
  return true;
}

NR_UE_RRC_INST_t* nr_rrc_init_ue(char* uecap_file, int nb_inst, int num_ant_tx)
{
  NR_UE_rrc_inst = (NR_UE_RRC_INST_t *)calloc(nb_inst, sizeof(NR_UE_RRC_INST_t));
  AssertFatal(NR_UE_rrc_inst, "Couldn't allocate %d instances of RRC module\n", nb_inst);

  for(int nr_ue = 0; nr_ue < nb_inst; nr_ue++) {
    NR_UE_RRC_INST_t *rrc = &NR_UE_rrc_inst[nr_ue];
    rrc->ue_id = nr_ue;
    // fill UE-NR-Capability @ UE-CapabilityRAT-Container here.
    rrc->selected_plmn_identity = 1;
    rrc->ra_trigger = RA_NOT_RUNNING;
    rrc->dl_bwp_id = 0;
    rrc->ul_bwp_id = 0;
    rrc->as_security_activated = false;
    rrc->detach_after_release = false;
    rrc->reconfig_after_reestab = false;
    /* 5G-S-TMSI */
    rrc->fiveG_S_TMSI = UINT64_MAX;

    FILE *f = NULL;
    if (uecap_file)
      f = fopen(uecap_file, "r");
    if (f) {
      char UE_NR_Capability_xer[65536];
      size_t size = fread(UE_NR_Capability_xer, 1, sizeof UE_NR_Capability_xer, f);
      if (size == 0 || size == sizeof UE_NR_Capability_xer) {
        LOG_E(NR_RRC, "UE Capabilities XER file %s is too large (%ld)\n", uecap_file, size);
      }
      else {
        asn_dec_rval_t dec_rval =
            xer_decode(0, &asn_DEF_NR_UE_NR_Capability, (void *)&rrc->UECap.UE_NR_Capability, UE_NR_Capability_xer, size);
        assert(dec_rval.code == RC_OK);
      }
      fclose(f);
      /* Verify consistency of num PHY antennas vs UE Capabilities */
      verify_ue_cap(rrc->UECap.UE_NR_Capability, num_ant_tx);
    }

    memset(&rrc->timers_and_constants, 0, sizeof(rrc->timers_and_constants));
    set_default_timers_and_constants(&rrc->timers_and_constants);

    for (int j = 0; j < NR_NUM_SRB; j++)
      rrc->Srb[j] = RB_NOT_PRESENT;
    for (int j = 1; j <= MAX_DRBS_PER_UE; j++)
      set_DRB_status(rrc, j, RB_NOT_PRESENT);
    // SRB0 activated by default
    rrc->Srb[0] = RB_ESTABLISHED;
    for (int j = 0; j < NR_MAX_NUM_LCID; j++)
      rrc->active_RLC_entity[j] = false;

    for (int i = 0; i < NB_CNX_UE; i++) {
      rrcPerNB_t *ptr = &rrc->perNB[i];
      ptr->SInfo = (NR_UE_RRC_SI_INFO){0};
      init_SI_timers(&ptr->SInfo);
    }

    init_sidelink(rrc);
  }

  return NR_UE_rrc_inst;
}

bool check_si_validity(NR_UE_RRC_SI_INFO *SI_info, int si_type)
{
  switch (si_type) {
    case NR_SIB_TypeInfo__type_sibType2:
      if (SI_info->sib2_validity == SIB_NOT_VALID) {
        SI_info->sib2_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType3:
      if (SI_info->sib3_validity == SIB_NOT_VALID) {
        SI_info->sib3_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType4:
      if (SI_info->sib4_validity == SIB_NOT_VALID) {
        SI_info->sib4_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType5:
      if (SI_info->sib5_validity == SIB_NOT_VALID) {
        SI_info->sib5_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType6:
      if (SI_info->sib6_validity == SIB_NOT_VALID) {
        SI_info->sib6_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType7:
      if (SI_info->sib7_validity == SIB_NOT_VALID) {
        SI_info->sib7_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType8:
      if (SI_info->sib8_validity == SIB_NOT_VALID) {
        SI_info->sib8_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType9:
      if (SI_info->sib9_validity == SIB_NOT_VALID) {
        SI_info->sib9_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType10_v1610:
      if (SI_info->sib10_validity == SIB_NOT_VALID) {
        SI_info->sib10_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType11_v1610:
      if (SI_info->sib11_validity == SIB_NOT_VALID) {
        SI_info->sib11_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType12_v1610:
      if (SI_info->sib12_validity == SIB_NOT_VALID) {
        SI_info->sib12_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType13_v1610:
      if (SI_info->sib13_validity == SIB_NOT_VALID) {
        SI_info->sib13_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo__type_sibType14_v1610:
      if (SI_info->sib14_validity == SIB_NOT_VALID) {
        SI_info->sib14_validity = SIB_REQUESTED;
        return false;
      }
      break;
    default :
      AssertFatal(false, "Invalid SIB type %d\n", si_type);
  }
  return true;
}

bool check_si_validity_r17(NR_UE_RRC_SI_INFO_r17 *SI_info, int si_type)
{
  switch (si_type) {
    case NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType15:
      if (SI_info->sib15_validity == SIB_NOT_VALID) {
        SI_info->sib15_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType16:
      if (SI_info->sib16_validity == SIB_NOT_VALID) {
        SI_info->sib16_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType17:
      if (SI_info->sib17_validity == SIB_NOT_VALID) {
        SI_info->sib17_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType18:
      if (SI_info->sib18_validity == SIB_NOT_VALID) {
        SI_info->sib18_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType19:
      if (SI_info->sib19_validity == SIB_NOT_VALID) {
        SI_info->sib19_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType20:
      if (SI_info->sib20_validity == SIB_NOT_VALID) {
        SI_info->sib20_validity = SIB_REQUESTED;
        return false;
      }
      break;
    case NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType21:
      if (SI_info->sib21_validity == SIB_NOT_VALID) {
        SI_info->sib21_validity = SIB_REQUESTED;
        return false;
      }
      break;
    default :
      AssertFatal(false, "Invalid SIB r17 type %d\n", si_type);
  }
  return true;
}

int check_si_status(NR_UE_RRC_SI_INFO *SI_info)
{
  // schedule reception of SIB1 if RRC doesn't have it
  if (SI_info->sib1_validity == SIB_NOT_VALID) {
    SI_info->sib1_validity = SIB_REQUESTED;
    return 1;
  }
  else {
    if (SI_info->default_otherSI_map) {
      // Check if RRC has configured default SI
      // from SIB2 to SIB14 as current ASN1 version
      // TODO can be used for on demand SI when (if) implemented
      for (int i = 2; i < 15; i++) {
        int si_index = i - 2;
        if ((SI_info->default_otherSI_map >> si_index) & 0x01) {
          // if RRC has no valid version of one of the default configured SI
          // Then schedule reception of otherSI
          if (!check_si_validity(SI_info, si_index))
            return 2;
        }
      }
    }

    // Check if RRC has configured default SI
    // from SIB15 to SIB21 as current r17 version
    if (SI_info->SInfo_r17.default_otherSI_map_r17) {
      for (int i = 15; i < 22; i++) {
        int si_index = i - 15;
        if ((SI_info->SInfo_r17.default_otherSI_map_r17 >> si_index) & 0x01) {
          // if RRC has no valid version of one of the default configured SI
          // Then schedule reception of otherSI
          if (!check_si_validity_r17(&SI_info->SInfo_r17, si_index))
            return 2;
        }
      }
    }
  }
  return 0;
}

/*brief decode BCCH-BCH (MIB) message*/
static void nr_rrc_ue_decode_NR_BCCH_BCH_Message(NR_UE_RRC_INST_t *rrc,
                                                 const uint8_t gNB_index,
                                                 const uint32_t phycellid,
                                                 const long ssb_arfcn,
                                                 uint8_t *const bufferP,
                                                 const uint8_t buffer_len)
{
  NR_BCCH_BCH_Message_t *bcch_message = NULL;
  rrc->phyCellID = phycellid;
  rrc->arfcn_ssb = ssb_arfcn;

  asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                 &asn_DEF_NR_BCCH_BCH_Message,
                                                 (void **)&bcch_message,
                                                 (const void *)bufferP,
                                                 buffer_len);

  if ((dec_rval.code != RC_OK) || (dec_rval.consumed == 0)) {
    LOG_E(NR_RRC, "NR_BCCH_BCH decode error\n");
    return;
  }
  if (LOG_DEBUGFLAG(DEBUG_ASN1))
    xer_fprint(stdout, &asn_DEF_NR_BCCH_BCH_Message, (void *)bcch_message);
    
  // Actions following cell selection while T311 is running
  NR_UE_Timers_Constants_t *timers = &rrc->timers_and_constants;
  if (nr_timer_is_active(&timers->T311)) {
    nr_timer_stop(&timers->T311);
    rrc->ra_trigger = RRC_CONNECTION_REESTABLISHMENT;

    // preparing MSG3 for re-establishment in advance
    uint8_t buffer[1024];
    int buf_size = do_RRCReestablishmentRequest(buffer,
                                                rrc->reestablishment_cause,
                                                rrc->phyCellID,
                                                rrc->rnti); // old rnti

    nr_rlc_srb_recv_sdu(rrc->ue_id, 0, buffer, buf_size);

    // apply the default MAC Cell Group configuration
    // (done at MAC by calling nr_ue_mac_default_configs)

    // apply the timeAlignmentTimerCommon included in SIB1
    // not used
  }

  int get_sib = 0;
  if (IS_SA_MODE(get_softmodem_params()) && bcch_message->message.present == NR_BCCH_BCH_MessageType_PR_mib
      && bcch_message->message.choice.mib->cellBarred == NR_MIB__cellBarred_notBarred && rrc->nrRrcState != RRC_STATE_DETACH_NR) {
    NR_UE_RRC_SI_INFO *SI_info = &rrc->perNB[gNB_index].SInfo;
    // to schedule MAC to get SI if required
    get_sib = check_si_status(SI_info);
  }
  if (bcch_message->message.present == NR_BCCH_BCH_MessageType_PR_mib) {
    MessageDef *msg = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_MAC_RRC_CONFIG_MIB);
    // mac will manage the pointer
    NR_MAC_RRC_CONFIG_MIB(msg).bcch = bcch_message;
    NR_MAC_RRC_CONFIG_MIB(msg).get_sib = get_sib;
    itti_send_msg_to_task(TASK_MAC_UE, rrc->ue_id, msg);
  } else {
    LOG_E(NR_RRC, "RRC-received BCCH message is not a MIB\n");
    ASN_STRUCT_FREE(asn_DEF_NR_BCCH_BCH_Message, bcch_message);
  }
  return;
}

static void nr_rrc_handle_msg3_indication(NR_UE_RRC_INST_t *rrc, rnti_t rnti)
{
  NR_UE_Timers_Constants_t *tac = &rrc->timers_and_constants;
  switch (rrc->ra_trigger) {
    case RRC_CONNECTION_SETUP:
      // After SIB1 is received, prepare RRCConnectionRequest
      rrc->rnti = rnti;
      // start timer T300
      nr_timer_start(&tac->T300);
      break;
    case RRC_CONNECTION_REESTABLISHMENT:
      rrc->rnti = rnti;
      nr_timer_start(&tac->T301);
      int srb_id = 1;
      // re-establish PDCP for SRB1
      // (and suspend integrity protection and ciphering for SRB1)
      nr_pdcp_entity_security_keys_and_algos_t null_security_parameters = {0};
      nr_pdcp_reestablishment(rrc->ue_id, srb_id, true, &null_security_parameters);
      // re-establish RLC for SRB1
      int lc_id = nr_rlc_get_lcid_from_rb(rrc->ue_id, true, 1);
      nr_rlc_reestablish_entity(rrc->ue_id, lc_id);
      // apply the specified configuration defined in 9.2.1 for SRB1
      nr_rlc_reconfigure_entity(rrc->ue_id, lc_id, NULL);
      // resume SRB1
      rrc->Srb[srb_id] = RB_ESTABLISHED;
      break;
    case DURING_HANDOVER:
      AssertFatal(1==0, "ra_trigger not implemented yet!\n");
      break;
    case NON_SYNCHRONISED:
      AssertFatal(1==0, "ra_trigger not implemented yet!\n");
      break;
    case TRANSITION_FROM_RRC_INACTIVE:
      AssertFatal(1==0, "ra_trigger not implemented yet!\n");
      break;
    case TO_ESTABLISH_TA:
      AssertFatal(1==0, "ra_trigger not implemented yet!\n");
      break;
    case REQUEST_FOR_OTHER_SI:
      AssertFatal(1==0, "ra_trigger not implemented yet!\n");
      break;
    case BEAM_FAILURE_RECOVERY:
      AssertFatal(1==0, "ra_trigger not implemented yet!\n");
      break;
    default:
      AssertFatal(1==0, "Invalid ra_trigger value!\n");
      break;
  }
}

static int8_t nr_rrc_ue_decode_NR_BCCH_DL_SCH_Message(NR_UE_RRC_INST_t *rrc,
                                                      const uint8_t gNB_index,
                                                      uint8_t *const Sdu,
                                                      const uint8_t Sdu_len,
                                                      const uint8_t rsrq,
                                                      const uint8_t rsrp)
{
  NR_BCCH_DL_SCH_Message_t *bcch_message = NULL;

  NR_UE_RRC_SI_INFO *SI_info = &rrc->perNB[gNB_index].SInfo;
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_UE_DECODE_BCCH, VCD_FUNCTION_IN);

  asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                 &asn_DEF_NR_BCCH_DL_SCH_Message,
                                                 (void **)&bcch_message,
                                                 (const void *)Sdu,
                                                 Sdu_len);

  if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
    LOG_E(NR_RRC, "[UE %ld] Failed to decode BCCH_DLSCH_MESSAGE (%zu bits)\n", rrc->ue_id, dec_rval.consumed);
    log_dump(NR_RRC, Sdu, Sdu_len, LOG_DUMP_CHAR,"   Received bytes:\n");
    // free the memory
    SEQUENCE_free(&asn_DEF_NR_BCCH_DL_SCH_Message, (void *)bcch_message, 1);
    VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_UE_DECODE_BCCH, VCD_FUNCTION_OUT );
    return -1;
  }

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_BCCH_DL_SCH_Message,(void *)bcch_message);
  }

  if (bcch_message->message.present == NR_BCCH_DL_SCH_MessageType_PR_c1) {
    switch (bcch_message->message.choice.c1->present) {
      case NR_BCCH_DL_SCH_MessageType__c1_PR_systemInformationBlockType1:
        nr_rrc_process_sib1(rrc, SI_info, bcch_message->message.choice.c1->choice.systemInformationBlockType1);
        // mac layer will free after usage the sib1
        bcch_message->message.choice.c1->choice.systemInformationBlockType1 = NULL;
        break;
      case NR_BCCH_DL_SCH_MessageType__c1_PR_systemInformation:
        LOG_I(NR_RRC, "[UE %ld] Decoding SI\n", rrc->ue_id);
        NR_SystemInformation_t *si = bcch_message->message.choice.c1->choice.systemInformation;
        nr_decode_SI(SI_info, si, rrc);
        break;
      case NR_BCCH_DL_SCH_MessageType__c1_PR_NOTHING:
      default:
        break;
    }
  }
  SEQUENCE_free(&asn_DEF_NR_BCCH_DL_SCH_Message, bcch_message, ASFM_FREE_EVERYTHING);
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_UE_DECODE_BCCH, VCD_FUNCTION_OUT );
  return 0;
}

static void nr_rrc_signal_maxrtxindication(int ue_id)
{
  MessageDef *msg = itti_alloc_new_message(TASK_RLC_UE, ue_id, NR_RRC_RLC_MAXRTX);
  NR_RRC_RLC_MAXRTX(msg).ue_id = ue_id;
  itti_send_msg_to_task(TASK_RRC_NRUE, ue_id, msg);
}

static void nr_rrc_manage_rlc_bearers(NR_UE_RRC_INST_t *rrc,
                                      const NR_CellGroupConfig_t *cellGroupConfig)
{
  if (cellGroupConfig->rlc_BearerToReleaseList != NULL) {
    for (int i = 0; i < cellGroupConfig->rlc_BearerToReleaseList->list.count; i++) {
      NR_LogicalChannelIdentity_t *lcid = cellGroupConfig->rlc_BearerToReleaseList->list.array[i];
      AssertFatal(lcid, "LogicalChannelIdentity shouldn't be null here\n");
      nr_rlc_release_entity(rrc->ue_id, *lcid);
    }
  }

  if (cellGroupConfig->rlc_BearerToAddModList != NULL) {
    for (int i = 0; i < cellGroupConfig->rlc_BearerToAddModList->list.count; i++) {
      NR_RLC_BearerConfig_t *rlc_bearer = cellGroupConfig->rlc_BearerToAddModList->list.array[i];
      NR_LogicalChannelIdentity_t lcid = rlc_bearer->logicalChannelIdentity;
      if (rrc->active_RLC_entity[lcid]) {
        if (rlc_bearer->reestablishRLC)
          nr_rlc_reestablish_entity(rrc->ue_id, lcid);
        if (rlc_bearer->rlc_Config)
          nr_rlc_reconfigure_entity(rrc->ue_id, lcid, rlc_bearer->rlc_Config);
      } else {
        rrc->active_RLC_entity[lcid] = true;
        AssertFatal(rlc_bearer->servedRadioBearer, "servedRadioBearer mandatory in case of setup\n");
        AssertFatal(rlc_bearer->servedRadioBearer->present != NR_RLC_BearerConfig__servedRadioBearer_PR_NOTHING,
                    "Invalid RB for RLC configuration\n");
        if (rlc_bearer->servedRadioBearer->present == NR_RLC_BearerConfig__servedRadioBearer_PR_srb_Identity) {
          NR_SRB_Identity_t srb_id = rlc_bearer->servedRadioBearer->choice.srb_Identity;
          nr_rlc_add_srb(rrc->ue_id, srb_id, rlc_bearer);
          nr_rlc_set_rlf_handler(rrc->ue_id, nr_rrc_signal_maxrtxindication);
        } else { // DRB
          NR_DRB_Identity_t drb_id = rlc_bearer->servedRadioBearer->choice.drb_Identity;
          nr_rlc_add_drb(rrc->ue_id, drb_id, rlc_bearer);
          nr_rlc_set_rlf_handler(rrc->ue_id, nr_rrc_signal_maxrtxindication);
        }
      }
    }
  }
}

static void nr_rrc_process_reconfigurationWithSync(NR_UE_RRC_INST_t *rrc, NR_ReconfigurationWithSync_t *reconfigurationWithSync)
{
  // perform Reconfiguration with sync according to 5.3.5.5.2
  if (!rrc->as_security_activated && !(get_softmodem_params()->phy_test || get_softmodem_params()->do_ra)) {
    // if the AS security is not activated, perform the actions upon going to RRC_IDLE as specified in 5.3.11
    // with the release cause 'other' upon which the procedure ends
    NR_Release_Cause_t release_cause = OTHER;
    nr_rrc_going_to_IDLE(rrc, release_cause, NULL);
    return;
  }

  if (reconfigurationWithSync->spCellConfigCommon &&
      reconfigurationWithSync->spCellConfigCommon->downlinkConfigCommon &&
      reconfigurationWithSync->spCellConfigCommon->downlinkConfigCommon->frequencyInfoDL &&
      reconfigurationWithSync->spCellConfigCommon->downlinkConfigCommon->frequencyInfoDL->absoluteFrequencySSB)
    rrc->arfcn_ssb = *reconfigurationWithSync->spCellConfigCommon->downlinkConfigCommon->frequencyInfoDL->absoluteFrequencySSB;

  NR_UE_Timers_Constants_t *tac = &rrc->timers_and_constants;
  nr_timer_stop(&tac->T310);
  if (!get_softmodem_params()->phy_test) {
    // T304 is stopped upon completion of RA procedure which is not done in phy-test mode
    int t304_value = nr_rrc_get_T304(reconfigurationWithSync->t304);
    nr_timer_setup(&tac->T304, t304_value, 10); // 10ms step
    nr_timer_start(&tac->T304);
  }
  rrc->rnti = reconfigurationWithSync->newUE_Identity;
  // reset the MAC entity of this cell group (done at MAC in handle_reconfiguration_with_sync)
}

void nr_rrc_cellgroup_configuration(NR_UE_RRC_INST_t *rrc, NR_CellGroupConfig_t *cellGroupConfig)
{
  NR_SpCellConfig_t *spCellConfig = cellGroupConfig->spCellConfig;
  if(spCellConfig) {
    if (spCellConfig->reconfigurationWithSync) {
      LOG_I(NR_RRC, "Processing reconfigurationWithSync\n");
      nr_rrc_process_reconfigurationWithSync(rrc, spCellConfig->reconfigurationWithSync);
    }
    nr_rrc_handle_SetupRelease_RLF_TimersAndConstants(rrc, spCellConfig->rlf_TimersAndConstants);
    if (spCellConfig->spCellConfigDedicated) {
      if (spCellConfig->spCellConfigDedicated->firstActiveDownlinkBWP_Id)
        rrc->dl_bwp_id = *spCellConfig->spCellConfigDedicated->firstActiveDownlinkBWP_Id;
      if (spCellConfig->spCellConfigDedicated->uplinkConfig &&
          spCellConfig->spCellConfigDedicated->uplinkConfig->firstActiveUplinkBWP_Id)
        rrc->dl_bwp_id = *spCellConfig->spCellConfigDedicated->uplinkConfig->firstActiveUplinkBWP_Id;
    }
  }

  nr_rrc_manage_rlc_bearers(rrc, cellGroupConfig);

  if (cellGroupConfig->ext1)
    AssertFatal(cellGroupConfig->ext1->reportUplinkTxDirectCurrent == NULL, "Reporting of UplinkTxDirectCurrent not implemented\n");
  AssertFatal(cellGroupConfig->sCellToReleaseList == NULL, "Secondary serving cell release not implemented\n");
  AssertFatal(cellGroupConfig->sCellToAddModList == NULL, "Secondary serving cell addition not implemented\n");
}


static void nr_rrc_ue_process_masterCellGroup(NR_UE_RRC_INST_t *rrc,
                                              OCTET_STRING_t *masterCellGroup,
                                              long *fullConfig)
{
  AssertFatal(!fullConfig, "fullConfig not supported yet\n");
  NR_CellGroupConfig_t *cellGroupConfig = NULL;
  uper_decode(NULL,
              &asn_DEF_NR_CellGroupConfig,   //might be added prefix later
              (void **)&cellGroupConfig,
              (uint8_t *)masterCellGroup->buf,
              masterCellGroup->size, 0, 0);

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_CellGroupConfig, (const void *) cellGroupConfig);
  }

  nr_rrc_cellgroup_configuration(rrc, cellGroupConfig);

  LOG_D(RRC, "Sending CellGroupConfig to MAC the pointer will be managed by mac\n");
  MessageDef *msg = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_MAC_RRC_CONFIG_CG);
  NR_MAC_RRC_CONFIG_CG(msg).cellGroupConfig = cellGroupConfig;
  NR_MAC_RRC_CONFIG_CG(msg).UE_NR_Capability = rrc->UECap.UE_NR_Capability;
  itti_send_msg_to_task(TASK_MAC_UE, rrc->ue_id, msg);
}

static void rrc_ue_generate_RRCSetupComplete(const NR_UE_RRC_INST_t *rrc, const uint8_t Transaction_id)
{
  uint8_t buffer[100];
  as_nas_info_t initialNasMsg;

  if (IS_SA_MODE(get_softmodem_params())) {
    nr_ue_nas_t *nas = get_ue_nas_info(rrc->ue_id);
    // Send Initial NAS message (Registration Request) before Security Mode control procedure
    generateRegistrationRequest(&initialNasMsg, nas, false);
    if (!initialNasMsg.nas_data) {
      LOG_E(NR_RRC, "Failed to complete RRCSetup. NAS InitialUEMessage message not found.\n");
      return;
    }
  } else {
    initialNasMsg.length = sizeof(nr_nas_attach_req_imsi_dummy_NSA_case);
    initialNasMsg.nas_data = malloc_or_fail(initialNasMsg.length);
    memcpy(initialNasMsg.nas_data, nr_nas_attach_req_imsi_dummy_NSA_case, initialNasMsg.length);
  }

  // Encode RRCSetupComplete
  int size = do_RRCSetupComplete(buffer,
                                 sizeof(buffer),
                                 Transaction_id,
                                 rrc->selected_plmn_identity,
                                 rrc->ra_trigger == RRC_CONNECTION_SETUP,
                                 rrc->fiveG_S_TMSI,
                                 (const uint32_t)initialNasMsg.length,
                                 (const char*)initialNasMsg.nas_data);

  // Free dynamically allocated data (heap allocated in both SA and NSA)
  free(initialNasMsg.nas_data);

  LOG_I(NR_RRC, "[UE %ld][RAPROC] Logical Channel UL-DCCH (SRB1), Generating RRCSetupComplete (bytes%d)\n", rrc->ue_id, size);
  int srb_id = 1; // RRC setup complete on SRB1
  LOG_D(NR_RRC, "[RRC_UE %ld] PDCP_DATA_REQ/%d Bytes RRCSetupComplete ---> %d\n", rrc->ue_id, size, srb_id);
  nr_pdcp_data_req_srb(rrc->ue_id, srb_id, 0, size, buffer, deliver_pdu_srb_rlc, NULL);
}

static void nr_rrc_process_rrcsetup(NR_UE_RRC_INST_t *rrc,
                                    const NR_RRCSetup_t *rrcSetup)
{
  // if the RRCSetup is received in response to an RRCReestablishmentRequest
  // or RRCResumeRequest or RRCResumeRequest1
  // TODO none of the procedures implemented yet
  if (rrc->ra_trigger == RRC_CONNECTION_REESTABLISHMENT) {
    LOG_E(NR_RRC, "Handling of RRCSetup in response of RRCReestablishment not implemented yet. Going back to IDLE.\n");
    nr_rrc_going_to_IDLE(rrc, OTHER, NULL);
    return;
  }

  // perform the cell group configuration procedure in accordance with the received masterCellGroup
  nr_rrc_ue_process_masterCellGroup(rrc,
                                    &rrcSetup->criticalExtensions.choice.rrcSetup->masterCellGroup,
                                    NULL);
  // perform the radio bearer configuration procedure in accordance with the received radioBearerConfig
  nr_rrc_ue_process_RadioBearerConfig(rrc,
                                      &rrcSetup->criticalExtensions.choice.rrcSetup->radioBearerConfig);

  // TODO (not handled) if stored, discard the cell reselection priority information provided by
  // the cellReselectionPriorities or inherited from another RAT

  // stop timer T300, T301, T319, T320 if running;
  NR_UE_Timers_Constants_t *timers = &rrc->timers_and_constants;
  nr_timer_stop(&timers->T300);
  nr_timer_stop(&timers->T301);
  nr_timer_stop(&timers->T319);
  nr_timer_stop(&timers->T320);

  // TODO if T390 and T302 are running (not implemented)

  // if the RRCSetup is received in response to an RRCResumeRequest, RRCResumeRequest1 or RRCSetupRequest
  // enter RRC_CONNECTED
  rrc->nrRrcState = RRC_STATE_CONNECTED_NR;

  // Indicate to NAS that the RRC connection has been established (5.3.1.3 of 3GPP TS 24.501)
  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_NAS_CONN_ESTABLISH_IND);
  itti_send_msg_to_task(TASK_NAS_NRUE, rrc->ue_id, msg_p);

  // set the content of RRCSetupComplete message
  // TODO procedues described in 5.3.3.4 seems more complex than what we actualy do
  rrc_ue_generate_RRCSetupComplete(rrc, rrcSetup->rrc_TransactionIdentifier);
}

static int8_t nr_rrc_ue_decode_ccch(NR_UE_RRC_INST_t *rrc,
                                    const NRRrcMacCcchDataInd *ind)
{
  NR_DL_CCCH_Message_t *dl_ccch_msg = NULL;
  asn_dec_rval_t dec_rval;
  int rval=0;
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_UE_DECODE_CCCH, VCD_FUNCTION_IN);
  LOG_D(RRC, "[NR UE%ld] Decoding DL-CCCH message (%d bytes), State %d\n", rrc->ue_id, ind->sdu_size, rrc->nrRrcState);

  dec_rval = uper_decode(NULL, &asn_DEF_NR_DL_CCCH_Message, (void **)&dl_ccch_msg, ind->sdu, ind->sdu_size, 0, 0);

  if (LOG_DEBUGFLAG(DEBUG_ASN1))
    xer_fprint(stdout, &asn_DEF_NR_DL_CCCH_Message, (void *)dl_ccch_msg);

  if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
    LOG_E(RRC, "[UE %ld] Failed to decode DL-CCCH-Message (%zu bytes)\n", rrc->ue_id, dec_rval.consumed);
    VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_UE_DECODE_CCCH, VCD_FUNCTION_OUT);
    return -1;
   }

   if (dl_ccch_msg->message.present == NR_DL_CCCH_MessageType_PR_c1) {
     switch (dl_ccch_msg->message.choice.c1->present) {
       case NR_DL_CCCH_MessageType__c1_PR_NOTHING:
         LOG_I(NR_RRC, "[UE%ld] Received PR_NOTHING on DL-CCCH-Message\n", rrc->ue_id);
         rval = 0;
         break;

       case NR_DL_CCCH_MessageType__c1_PR_rrcReject:
         LOG_I(NR_RRC, "[UE%ld] Logical Channel DL-CCCH (SRB0), Received RRCReject \n", rrc->ue_id);
         rval = 0;
         break;

       case NR_DL_CCCH_MessageType__c1_PR_rrcSetup:
         LOG_I(NR_RRC, "[UE%ld][RAPROC] Logical Channel DL-CCCH (SRB0), Received NR_RRCSetup\n", rrc->ue_id);
         nr_rrc_process_rrcsetup(rrc, dl_ccch_msg->message.choice.c1->choice.rrcSetup);
         rval = 0;
         break;

       default:
         LOG_E(NR_RRC, "[UE%ld] Unknown message\n", rrc->ue_id);
         rval = -1;
         break;
     }
   }

   ASN_STRUCT_FREE(asn_DEF_NR_DL_CCCH_Message, dl_ccch_msg);
   VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(VCD_SIGNAL_DUMPER_FUNCTIONS_UE_DECODE_CCCH, VCD_FUNCTION_OUT);
   return rval;
}

static void nr_rrc_ue_process_securityModeCommand(NR_UE_RRC_INST_t *ue_rrc,
                                                  NR_SecurityModeCommand_t *const securityModeCommand,
                                                  int srb_id,
                                                  const uint8_t *msg,
                                                  int msg_size,
                                                  const nr_pdcp_integrity_data_t *msg_integrity)
{
  LOG_I(NR_RRC, "Receiving from SRB1 (DL-DCCH), Processing securityModeCommand\n");

  AssertFatal(securityModeCommand->criticalExtensions.present == NR_SecurityModeCommand__criticalExtensions_PR_securityModeCommand,
        "securityModeCommand->criticalExtensions.present (%d) != "
        "NR_SecurityModeCommand__criticalExtensions_PR_securityModeCommand\n",
        securityModeCommand->criticalExtensions.present);

  NR_SecurityConfigSMC_t *securityConfigSMC =
      &securityModeCommand->criticalExtensions.choice.securityModeCommand->securityConfigSMC;

  switch (securityConfigSMC->securityAlgorithmConfig.cipheringAlgorithm) {
    case NR_CipheringAlgorithm_nea0:
    case NR_CipheringAlgorithm_nea1:
    case NR_CipheringAlgorithm_nea2:
      LOG_I(NR_RRC, "Security algorithm is set to nea%ld\n",
            securityConfigSMC->securityAlgorithmConfig.cipheringAlgorithm);
      break;
    default:
      AssertFatal(0, "Security algorithm not known/supported\n");
  }
  ue_rrc->cipheringAlgorithm = securityConfigSMC->securityAlgorithmConfig.cipheringAlgorithm;

  ue_rrc->integrityProtAlgorithm = 0;
  if (securityConfigSMC->securityAlgorithmConfig.integrityProtAlgorithm != NULL) {
    switch (*securityConfigSMC->securityAlgorithmConfig.integrityProtAlgorithm) {
      case NR_IntegrityProtAlgorithm_nia0:
      case NR_IntegrityProtAlgorithm_nia1:
      case NR_IntegrityProtAlgorithm_nia2:
        LOG_I(NR_RRC, "Integrity protection algorithm is set to nia%ld\n", *securityConfigSMC->securityAlgorithmConfig.integrityProtAlgorithm);
        break;
      default:
        AssertFatal(0, "Integrity algorithm not known/supported\n");
    }
    ue_rrc->integrityProtAlgorithm = *securityConfigSMC->securityAlgorithmConfig.integrityProtAlgorithm;
  }

  nr_pdcp_entity_security_keys_and_algos_t security_parameters;
  nr_derive_key(RRC_ENC_ALG, ue_rrc->cipheringAlgorithm, ue_rrc->kgnb, security_parameters.ciphering_key);
  nr_derive_key(RRC_INT_ALG, ue_rrc->integrityProtAlgorithm, ue_rrc->kgnb, security_parameters.integrity_key);

  log_dump(NR_RRC, ue_rrc->kgnb, 32, LOG_DUMP_CHAR, "deriving kRRCenc, kRRCint from KgNB=");

  /* for SecurityModeComplete, ciphering is not activated yet, only integrity */
  security_parameters.ciphering_algorithm = 0;
  security_parameters.integrity_algorithm = ue_rrc->integrityProtAlgorithm;
  // configure lower layers to apply SRB integrity protection and ciphering
  for (int i = 1; i < NR_NUM_SRB; i++) {
    if (ue_rrc->Srb[i] == RB_ESTABLISHED)
      nr_pdcp_config_set_security(ue_rrc->ue_id, i, true, &security_parameters);
  }

  NR_UL_DCCH_Message_t ul_dcch_msg = {0};

  ul_dcch_msg.message.present = NR_UL_DCCH_MessageType_PR_c1;
  asn1cCalloc(ul_dcch_msg.message.choice.c1, c1);

  // the SecurityModeCommand message needs to pass the integrity protection check
  // for the UE to declare AS security to be activated
  bool integrity_pass = nr_pdcp_check_integrity_srb(ue_rrc->ue_id, srb_id, msg, msg_size, msg_integrity);
  if (!integrity_pass) {
    /* - continue using the configuration used prior to the reception of the SecurityModeCommand message, i.e.
     *   neither apply integrity protection nor ciphering.
     * - submit the SecurityModeFailure message to lower layers for transmission, upon which the procedure ends.
     */
    LOG_E(NR_RRC, "integrity of SecurityModeCommand failed, reply with SecurityModeFailure\n");
    c1->present = NR_UL_DCCH_MessageType__c1_PR_securityModeFailure;
    asn1cCalloc(c1->choice.securityModeFailure, modeFailure);
    modeFailure->rrc_TransactionIdentifier = securityModeCommand->rrc_TransactionIdentifier;
    modeFailure->criticalExtensions.present = NR_SecurityModeFailure__criticalExtensions_PR_securityModeFailure;
    asn1cCalloc(modeFailure->criticalExtensions.choice.securityModeFailure, ext);
    ext->nonCriticalExtension = NULL;

    uint8_t buffer[200];
    asn_enc_rval_t enc_rval =
        uper_encode_to_buffer(&asn_DEF_NR_UL_DCCH_Message, NULL, (void *)&ul_dcch_msg, buffer, sizeof(buffer));
    AssertFatal(enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %jd)!\n", enc_rval.failed_type->name, enc_rval.encoded);
    if (LOG_DEBUGFLAG(DEBUG_ASN1))
      xer_fprint(stdout, &asn_DEF_NR_UL_DCCH_Message, (void *)&ul_dcch_msg);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NR_UL_DCCH_Message, &ul_dcch_msg);

    /* disable both ciphering and integrity */
    nr_pdcp_entity_security_keys_and_algos_t null_security_parameters = {0};
    for (int i = 1; i < NR_NUM_SRB; i++) {
      if (ue_rrc->Srb[i] == RB_ESTABLISHED)
        nr_pdcp_config_set_security(ue_rrc->ue_id, i, true, &null_security_parameters);
    }

    srb_id = 1; // SecurityModeFailure in SRB1
    nr_pdcp_data_req_srb(ue_rrc->ue_id, srb_id, 0, (enc_rval.encoded + 7) / 8, buffer, deliver_pdu_srb_rlc, NULL);

    return;
  }

  /* integrity passed, send SecurityModeComplete */
  c1->present = NR_UL_DCCH_MessageType__c1_PR_securityModeComplete;

  asn1cCalloc(c1->choice.securityModeComplete, modeComplete);
  modeComplete->rrc_TransactionIdentifier = securityModeCommand->rrc_TransactionIdentifier;
  modeComplete->criticalExtensions.present = NR_SecurityModeComplete__criticalExtensions_PR_securityModeComplete;
  asn1cCalloc(modeComplete->criticalExtensions.choice.securityModeComplete, ext);
  ext->nonCriticalExtension = NULL;
  LOG_I(NR_RRC,
        "Receiving from SRB1 (DL-DCCH), encoding securityModeComplete, rrc_TransactionIdentifier: %ld\n",
        securityModeCommand->rrc_TransactionIdentifier);
  uint8_t buffer[200];
  asn_enc_rval_t enc_rval =
      uper_encode_to_buffer(&asn_DEF_NR_UL_DCCH_Message, NULL, (void *)&ul_dcch_msg, buffer, sizeof(buffer));
  AssertFatal(enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %jd)!\n", enc_rval.failed_type->name, enc_rval.encoded);

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_UL_DCCH_Message, (void *)&ul_dcch_msg);
  }
  log_dump(NR_RRC, buffer, 16, LOG_DUMP_CHAR, "securityModeComplete payload: ");
  LOG_D(NR_RRC, "securityModeComplete Encoded %zd bits (%zd bytes)\n", enc_rval.encoded, (enc_rval.encoded + 7) / 8);
  ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NR_UL_DCCH_Message, &ul_dcch_msg);

  for (int i = 0; i < (enc_rval.encoded + 7) / 8; i++) {
    LOG_T(NR_RRC, "%02x.", buffer[i]);
  }
  LOG_T(NR_RRC, "\n");

  ue_rrc->as_security_activated = true;
  srb_id = 1; // SecurityModeComplete in SRB1
  nr_pdcp_data_req_srb(ue_rrc->ue_id, srb_id, 0, (enc_rval.encoded + 7) / 8, buffer, deliver_pdu_srb_rlc, NULL);

  /* after encoding SecurityModeComplete we activate both ciphering and integrity */
  security_parameters.ciphering_algorithm = ue_rrc->cipheringAlgorithm;
  // configure lower layers to apply SRB integrity protection and ciphering
  for (int i = 1; i < NR_NUM_SRB; i++) {
    if (ue_rrc->Srb[i] == RB_ESTABLISHED)
      nr_pdcp_config_set_security(ue_rrc->ue_id, i, true, &security_parameters);
  }
}

static void handle_meas_reporting_remove(rrcPerNB_t *rrc, int id, NR_UE_Timers_Constants_t *timers)
{
  // remove the measurement reporting entry for this measId if included
  asn1cFreeStruc(asn_DEF_NR_VarMeasReport, rrc->MeasReport[id]);
  // TODO stop the periodical reporting timer or timer T321, whichever is running,
  // and reset the associated information (e.g. timeToTrigger) for this measId
  nr_timer_stop(&timers->T321);
}

static void handle_measobj_remove(rrcPerNB_t *rrc, struct NR_MeasObjectToRemoveList *remove_list, NR_UE_Timers_Constants_t *timers)
{
  // section 5.5.2.4 in 38.331
  for (int i = 0; i < remove_list->list.count; i++) {
    // for each measObjectId included in the received measObjectToRemoveList
    // that is part of measObjectList in the configuration
    NR_MeasObjectId_t id = *remove_list->list.array[i];
    if (rrc->MeasObj[id - 1]) {
      // remove the entry with the matching measObjectId from the measObjectList
      asn1cFreeStruc(asn_DEF_NR_MeasObjectToAddMod, rrc->MeasObj[id - 1]);
      // remove all measId associated with this measObjectId from the measIdList
      for (int j = 0; j < MAX_MEAS_ID; j++) {
        if (rrc->MeasId[j] && rrc->MeasId[j]->measObjectId == id) {
          asn1cFreeStruc(asn_DEF_NR_MeasIdToAddMod, rrc->MeasId[j]);
          handle_meas_reporting_remove(rrc, j, timers);
        }
      }
    }
  }
}

static void update_ssb_configmob(NR_SSB_ConfigMobility_t *source, NR_SSB_ConfigMobility_t *target)
{
  if (source->ssb_ToMeasure)
    HANDLE_SETUPRELEASE_IE(target->ssb_ToMeasure, source->ssb_ToMeasure, NR_SSB_ToMeasure_t, asn_DEF_NR_SSB_ToMeasure);
  target->deriveSSB_IndexFromCell = source->deriveSSB_IndexFromCell;
  if (source->ss_RSSI_Measurement)
    UPDATE_IE(target->ss_RSSI_Measurement, source->ss_RSSI_Measurement, NR_SS_RSSI_Measurement_t);
}

static void update_nr_measobj(NR_MeasObjectNR_t *source, NR_MeasObjectNR_t *target)
{
  UPDATE_IE(target->ssbFrequency, source->ssbFrequency, NR_ARFCN_ValueNR_t);
  UPDATE_IE(target->ssbSubcarrierSpacing, source->ssbSubcarrierSpacing, NR_SubcarrierSpacing_t);
  UPDATE_IE(target->smtc1, source->smtc1, NR_SSB_MTC_t);
  if (source->smtc2) {
    target->smtc2->periodicity = source->smtc2->periodicity;
    if (source->smtc2->pci_List)
      UPDATE_IE(target->smtc2->pci_List, source->smtc2->pci_List, struct NR_SSB_MTC2__pci_List);
  }
  else
    asn1cFreeStruc(asn_DEF_NR_SSB_MTC2, target->smtc2);
  UPDATE_IE(target->refFreqCSI_RS, source->refFreqCSI_RS, NR_ARFCN_ValueNR_t);
  if (source->referenceSignalConfig.ssb_ConfigMobility)
    update_ssb_configmob(source->referenceSignalConfig.ssb_ConfigMobility, target->referenceSignalConfig.ssb_ConfigMobility);
  UPDATE_IE(target->absThreshSS_BlocksConsolidation, source->absThreshSS_BlocksConsolidation, NR_ThresholdNR_t);
  UPDATE_IE(target->absThreshCSI_RS_Consolidation, source->absThreshCSI_RS_Consolidation, NR_ThresholdNR_t);
  UPDATE_IE(target->nrofSS_BlocksToAverage, source->nrofSS_BlocksToAverage, long);
  UPDATE_IE(target->nrofCSI_RS_ResourcesToAverage, source->nrofCSI_RS_ResourcesToAverage, long);
  target->quantityConfigIndex = source->quantityConfigIndex;
  target->offsetMO = source->offsetMO;
  if (source->cellsToRemoveList) {
    RELEASE_IE_FROMLIST(source->cellsToRemoveList, target->cellsToAddModList, physCellId);
  }
  if (source->cellsToAddModList) {
    if (!target->cellsToAddModList)
      target->cellsToAddModList = calloc(1, sizeof(*target->cellsToAddModList));
    ADDMOD_IE_FROMLIST(source->cellsToAddModList, target->cellsToAddModList, physCellId, NR_CellsToAddMod_t);
  }
  if (source->excludedCellsToRemoveList) {
    RELEASE_IE_FROMLIST(source->excludedCellsToRemoveList, target->excludedCellsToAddModList, pci_RangeIndex);
  }
  if (source->excludedCellsToAddModList) {
    if (!target->excludedCellsToAddModList)
      target->excludedCellsToAddModList = calloc(1, sizeof(*target->excludedCellsToAddModList));
    ADDMOD_IE_FROMLIST(source->excludedCellsToAddModList, target->excludedCellsToAddModList, pci_RangeIndex, NR_PCI_RangeElement_t);
  }
  if (source->allowedCellsToRemoveList) {
    RELEASE_IE_FROMLIST(source->allowedCellsToRemoveList, target->allowedCellsToAddModList, pci_RangeIndex);
  }
  if (source->allowedCellsToAddModList) {
    if (!target->allowedCellsToAddModList)
      target->allowedCellsToAddModList = calloc(1, sizeof(*target->allowedCellsToAddModList));
    ADDMOD_IE_FROMLIST(source->allowedCellsToAddModList, target->allowedCellsToAddModList, pci_RangeIndex, NR_PCI_RangeElement_t);
  }
  if (source->ext1) {
    UPDATE_IE(target->ext1->freqBandIndicatorNR, source->ext1->freqBandIndicatorNR, NR_FreqBandIndicatorNR_t);
    UPDATE_IE(target->ext1->measCycleSCell, source->ext1->measCycleSCell, long);
  }
}

static void handle_measobj_addmod(rrcPerNB_t *rrc, struct NR_MeasObjectToAddModList *addmod_list)
{
  // section 5.5.2.5 in 38.331
  for (int i = 0; i < addmod_list->list.count; i++) {
    NR_MeasObjectToAddMod_t *measObj = addmod_list->list.array[i];
    if (measObj->measObject.present != NR_MeasObjectToAddMod__measObject_PR_measObjectNR) {
      LOG_E(NR_RRC, "Cannot handle MeasObjt other than NR\n");
      continue;
    }
    NR_MeasObjectId_t id = measObj->measObjectId;
    if (rrc->MeasObj[id]) {
      update_nr_measobj(measObj->measObject.choice.measObjectNR, rrc->MeasObj[id]->measObject.choice.measObjectNR);
    }
    else {
      // add a new entry for the received measObject to the measObjectList
      UPDATE_IE(rrc->MeasObj[id], addmod_list->list.array[i], NR_MeasObjectToAddMod_t);
    }
  }
}

static void handle_reportconfig_remove(rrcPerNB_t *rrc,
                                       struct NR_ReportConfigToRemoveList *remove_list,
                                       NR_UE_Timers_Constants_t *timers)
{
  for (int i = 0; i < remove_list->list.count; i++) {
    NR_ReportConfigId_t id = *remove_list->list.array[i];
    // remove the entry with the matching reportConfigId from the reportConfigList
    asn1cFreeStruc(asn_DEF_NR_ReportConfigToAddMod, rrc->ReportConfig[id]);
    for (int j = 0; j < MAX_MEAS_ID; j++) {
      if (rrc->MeasId[j] && rrc->MeasId[j]->reportConfigId == id) {
        // remove all measId associated with the reportConfigId from the measIdList
        asn1cFreeStruc(asn_DEF_NR_MeasIdToAddMod, rrc->MeasId[j]);
        handle_meas_reporting_remove(rrc, j, timers);
      }
    }
  }
}

static void handle_reportconfig_addmod(rrcPerNB_t *rrc,
                                       struct NR_ReportConfigToAddModList *addmod_list,
                                       NR_UE_Timers_Constants_t *timers)
{
  for (int i = 0; i < addmod_list->list.count; i++) {
    NR_ReportConfigToAddMod_t *rep = addmod_list->list.array[i];
    if (rep->reportConfig.present != NR_ReportConfigToAddMod__reportConfig_PR_reportConfigNR) {
      LOG_E(NR_RRC, "Cannot handle reportConfig type other than NR\n");
      continue;
    }
    NR_ReportConfigId_t id = rep->reportConfigId;
    if (rrc->ReportConfig[id]) {
      for (int j = 0; j < MAX_MEAS_ID; j++) {
        // for each measId associated with this reportConfigId included in the measIdList
        if (rrc->MeasId[j] && rrc->MeasId[j]->reportConfigId == id)
          handle_meas_reporting_remove(rrc, j, timers);
      }
    }
    UPDATE_IE(rrc->ReportConfig[id], addmod_list->list.array[i], NR_ReportConfigToAddMod_t);
  }
}

static void handle_quantityconfig(rrcPerNB_t *rrc, NR_QuantityConfig_t *quantityConfig, NR_UE_Timers_Constants_t *timers)
{
  if (quantityConfig->quantityConfigNR_List) {
    for (int i = 0; i < quantityConfig->quantityConfigNR_List->list.count; i++) {
      NR_QuantityConfigNR_t *quantityNR = quantityConfig->quantityConfigNR_List->list.array[i];
      if (!rrc->QuantityConfig[i])
        rrc->QuantityConfig[i] = calloc(1, sizeof(*rrc->QuantityConfig[i]));
      rrc->QuantityConfig[i]->quantityConfigCell = quantityNR->quantityConfigCell;
      if (quantityNR->quantityConfigRS_Index)
        UPDATE_IE(rrc->QuantityConfig[i]->quantityConfigRS_Index, quantityNR->quantityConfigRS_Index, struct NR_QuantityConfigRS);
    }
  }
  for (int j = 0; j < MAX_MEAS_ID; j++) {
    // for each measId included in the measIdList
    if (rrc->MeasId[j])
      handle_meas_reporting_remove(rrc, j, timers);
  }
}

static void handle_measid_remove(rrcPerNB_t *rrc, struct NR_MeasIdToRemoveList *remove_list, NR_UE_Timers_Constants_t *timers)
{
  for (int i = 0; i < remove_list->list.count; i++) {
    NR_MeasId_t id = *remove_list->list.array[i];
    if (rrc->MeasId[id]) {
      asn1cFreeStruc(asn_DEF_NR_MeasIdToAddMod, rrc->MeasId[id]);
      handle_meas_reporting_remove(rrc, id, timers);
    }
  }
}

static void handle_measid_addmod(rrcPerNB_t *rrc, struct NR_MeasIdToAddModList *addmod_list, NR_UE_Timers_Constants_t *timers)
{
  for (int i = 0; i < addmod_list->list.count; i++) {
    NR_MeasId_t id = addmod_list->list.array[i]->measId;
    NR_ReportConfigId_t reportId = addmod_list->list.array[i]->reportConfigId;
    NR_MeasObjectId_t measObjectId = addmod_list->list.array[i]->measObjectId;
    UPDATE_IE(rrc->MeasId[id], addmod_list->list.array[i], NR_MeasIdToAddMod_t);
    handle_meas_reporting_remove(rrc, id, timers);
    if (rrc->ReportConfig[reportId]) {
      NR_ReportConfigToAddMod_t *report = rrc->ReportConfig[reportId];
      AssertFatal(report->reportConfig.present == NR_ReportConfigToAddMod__reportConfig_PR_reportConfigNR,
                  "Only NR config report is supported\n");
      NR_ReportConfigNR_t *reportNR = report->reportConfig.choice.reportConfigNR;
      // if the reportType is set to reportCGI in the reportConfig associated with this measId
      if (reportNR->reportType.present == NR_ReportConfigNR__reportType_PR_reportCGI) {
        if (rrc->MeasObj[measObjectId]) {
          if (rrc->MeasObj[measObjectId]->measObject.present == NR_MeasObjectToAddMod__measObject_PR_measObjectNR) {
            NR_MeasObjectNR_t *obj_nr = rrc->MeasObj[measObjectId]->measObject.choice.measObjectNR;
            NR_ARFCN_ValueNR_t freq = 0;
            if (obj_nr->ssbFrequency)
              freq = *obj_nr->ssbFrequency;
            else if (obj_nr->refFreqCSI_RS)
              freq = *obj_nr->refFreqCSI_RS;
            AssertFatal(freq > 0, "Invalid ARFCN frequency for this measurement object\n");
            if (freq > 2016666)
              nr_timer_setup(&timers->T321, 16000, 10); // 16 seconds for FR2
            else
              nr_timer_setup(&timers->T321, 2000, 10); // 2 seconds for FR1
          }
          else // EUTRA
            nr_timer_setup(&timers->T321, 1000, 10); // 1 second for EUTRA
          nr_timer_start(&timers->T321);
        }
      }
    }
  }
}

static void nr_rrc_ue_process_measConfig(rrcPerNB_t *rrc, NR_MeasConfig_t *const measConfig, NR_UE_Timers_Constants_t *timers)
{
  if (measConfig->measObjectToRemoveList)
    handle_measobj_remove(rrc, measConfig->measObjectToRemoveList, timers);

  if (measConfig->measObjectToAddModList)
    handle_measobj_addmod(rrc, measConfig->measObjectToAddModList);

  if (measConfig->reportConfigToRemoveList)
    handle_reportconfig_remove(rrc, measConfig->reportConfigToRemoveList, timers);

  if (measConfig->reportConfigToAddModList)
    handle_reportconfig_addmod(rrc, measConfig->reportConfigToAddModList, timers);

  if (measConfig->quantityConfig)
    handle_quantityconfig(rrc, measConfig->quantityConfig, timers);

  if (measConfig->measIdToRemoveList)
    handle_measid_remove(rrc, measConfig->measIdToRemoveList, timers);

  if (measConfig->measIdToAddModList)
    handle_measid_addmod(rrc, measConfig->measIdToAddModList, timers);

  AssertFatal(!measConfig->measGapConfig, "Measurement gaps not yet supported\n");
  AssertFatal(!measConfig->measGapSharingConfig, "Measurement gaps not yet supported\n");

  if (measConfig->s_MeasureConfig) {
    if (measConfig->s_MeasureConfig->present == NR_MeasConfig__s_MeasureConfig_PR_ssb_RSRP) {
      rrc->s_measure = measConfig->s_MeasureConfig->choice.ssb_RSRP;
    } else if (measConfig->s_MeasureConfig->present == NR_MeasConfig__s_MeasureConfig_PR_csi_RSRP) {
      rrc->s_measure = measConfig->s_MeasureConfig->choice.csi_RSRP;
    }
  }
}

/**
 * @brief add, modify and release SRBs and/or DRBs
 * @ref   3GPP TS 38.331
 */
static void nr_rrc_ue_process_RadioBearerConfig(NR_UE_RRC_INST_t *ue_rrc,
                                                NR_RadioBearerConfig_t *const radioBearerConfig)
{
  if (LOG_DEBUGFLAG(DEBUG_ASN1))
    xer_fprint(stdout, &asn_DEF_NR_RadioBearerConfig, (const void *)radioBearerConfig);

  if (radioBearerConfig->srb3_ToRelease) {
    nr_pdcp_release_srb(ue_rrc->ue_id, 3);
    ue_rrc->Srb[3] = RB_NOT_PRESENT;
  }

  nr_pdcp_entity_security_keys_and_algos_t security_rrc_parameters = {0};
  nr_pdcp_entity_security_keys_and_algos_t security_up_parameters = {0};

  if (ue_rrc->as_security_activated) {
    if (radioBearerConfig->securityConfig != NULL) {
      // When the field is not included, continue to use the currently configured keyToUse
      if (radioBearerConfig->securityConfig->keyToUse) {
        AssertFatal(*radioBearerConfig->securityConfig->keyToUse == NR_SecurityConfig__keyToUse_master,
                    "Secondary key usage seems not to be implemented\n");
        ue_rrc->keyToUse = *radioBearerConfig->securityConfig->keyToUse;
      }
      // When the field is not included, continue to use the currently configured security algorithm
      if (radioBearerConfig->securityConfig->securityAlgorithmConfig) {
        ue_rrc->cipheringAlgorithm = radioBearerConfig->securityConfig->securityAlgorithmConfig->cipheringAlgorithm;
        ue_rrc->integrityProtAlgorithm = *radioBearerConfig->securityConfig->securityAlgorithmConfig->integrityProtAlgorithm;
      }
    }
    security_rrc_parameters.ciphering_algorithm = ue_rrc->cipheringAlgorithm;
    security_rrc_parameters.integrity_algorithm = ue_rrc->integrityProtAlgorithm;
    nr_derive_key(RRC_ENC_ALG, ue_rrc->cipheringAlgorithm, ue_rrc->kgnb, security_rrc_parameters.ciphering_key);
    nr_derive_key(RRC_INT_ALG, ue_rrc->integrityProtAlgorithm, ue_rrc->kgnb, security_rrc_parameters.integrity_key);
    security_up_parameters.ciphering_algorithm = ue_rrc->cipheringAlgorithm;
    security_up_parameters.integrity_algorithm = ue_rrc->integrityProtAlgorithm;
    nr_derive_key(UP_ENC_ALG, ue_rrc->cipheringAlgorithm, ue_rrc->kgnb, security_up_parameters.ciphering_key);
    nr_derive_key(UP_INT_ALG, ue_rrc->integrityProtAlgorithm, ue_rrc->kgnb, security_up_parameters.integrity_key);
  }

  if (radioBearerConfig->srb_ToAddModList != NULL) {
    for (int cnt = 0; cnt < radioBearerConfig->srb_ToAddModList->list.count; cnt++) {
      struct NR_SRB_ToAddMod *srb = radioBearerConfig->srb_ToAddModList->list.array[cnt];
      if (ue_rrc->Srb[srb->srb_Identity] == RB_NOT_PRESENT) {
        ue_rrc->Srb[srb->srb_Identity] = RB_ESTABLISHED;
        add_srb(false,
                ue_rrc->ue_id,
                radioBearerConfig->srb_ToAddModList->list.array[cnt],
                &security_rrc_parameters);
      }
      else {
        AssertFatal(srb->discardOnPDCP == NULL, "discardOnPDCP not yet implemented\n");
        if (srb->reestablishPDCP) {
          ue_rrc->Srb[srb->srb_Identity] = RB_ESTABLISHED;
          nr_pdcp_reestablishment(ue_rrc->ue_id,
                                  srb->srb_Identity,
                                  true,
                                  &security_rrc_parameters);
        }
        if (srb->pdcp_Config && srb->pdcp_Config->t_Reordering)
          nr_pdcp_reconfigure_srb(ue_rrc->ue_id, srb->srb_Identity, *srb->pdcp_Config->t_Reordering);
      }
    }
  }

  if (radioBearerConfig->drb_ToReleaseList) {
    for (int cnt = 0; cnt < radioBearerConfig->drb_ToReleaseList->list.count; cnt++) {
      NR_DRB_Identity_t *DRB_id = radioBearerConfig->drb_ToReleaseList->list.array[cnt];
      if (DRB_id) {
        nr_pdcp_release_drb(ue_rrc->ue_id, *DRB_id);
        set_DRB_status(ue_rrc, *DRB_id, RB_NOT_PRESENT);
      }
    }
  }

  /**
   * Establish/reconfig DRBs if DRB-ToAddMod is present
   * according to 3GPP TS 38.331 clause 5.3.5.6.5 DRB addition/modification
   */
  if (radioBearerConfig->drb_ToAddModList != NULL) {
    for (int cnt = 0; cnt < radioBearerConfig->drb_ToAddModList->list.count; cnt++) {
      struct NR_DRB_ToAddMod *drb = radioBearerConfig->drb_ToAddModList->list.array[cnt];
      int DRB_id = drb->drb_Identity;
      if (get_DRB_status(ue_rrc, DRB_id) != RB_NOT_PRESENT) {
        if (drb->reestablishPDCP) {
          set_DRB_status(ue_rrc, DRB_id, RB_ESTABLISHED);
          /* get integrity and cipehring settings from radioBearerConfig */
          bool has_integrity = drb->pdcp_Config != NULL
                               && drb->pdcp_Config->drb != NULL
                               && drb->pdcp_Config->drb->integrityProtection != NULL;
          bool has_ciphering = !(drb->pdcp_Config != NULL
                                 && drb->pdcp_Config->ext1 != NULL
                                 && drb->pdcp_Config->ext1->cipheringDisabled != NULL);
          security_up_parameters.ciphering_algorithm = has_ciphering ? ue_rrc->cipheringAlgorithm : 0;
          security_up_parameters.integrity_algorithm = has_integrity ? ue_rrc->integrityProtAlgorithm : 0;
          /* re-establish */
          nr_pdcp_reestablishment(ue_rrc->ue_id,
                                  DRB_id,
                                  false,
                                  &security_up_parameters);
        }
        AssertFatal(drb->recoverPDCP == NULL, "recoverPDCP not yet implemented\n");
        /* sdap-Config is included (SA mode) */
        NR_SDAP_Config_t *sdap_Config = drb->cnAssociation ? drb->cnAssociation->choice.sdap_Config : NULL;
        /* PDCP reconfiguration */
        if (drb->pdcp_Config)
          nr_pdcp_reconfigure_drb(ue_rrc->ue_id, DRB_id, drb->pdcp_Config);
        /* SDAP entity reconfiguration */
        if (sdap_Config)
          nr_reconfigure_sdap_entity(sdap_Config, ue_rrc->ue_id, sdap_Config->pdu_Session, DRB_id);
      } else {
        set_DRB_status(ue_rrc ,DRB_id, RB_ESTABLISHED);
        add_drb(false,
                ue_rrc->ue_id,
                radioBearerConfig->drb_ToAddModList->list.array[cnt],
                &security_up_parameters);
      }
    }
  } // drb_ToAddModList //

  ue_rrc->nrRrcState = RRC_STATE_CONNECTED_NR;
  LOG_I(NR_RRC, "State = NR_RRC_CONNECTED\n");
}

static void nr_rrc_ue_generate_RRCReconfigurationComplete(NR_UE_RRC_INST_t *rrc, const int srb_id, const uint8_t Transaction_id)
{
  uint8_t buffer[32];
  int size = do_NR_RRCReconfigurationComplete(buffer, sizeof(buffer), Transaction_id);
  LOG_I(NR_RRC, " Logical Channel UL-DCCH (SRB1), Generating RRCReconfigurationComplete (bytes %d)\n", size);
  AssertFatal(srb_id == 1 || srb_id == 3, "Invalid SRB ID %d\n", srb_id);
  LOG_D(RLC,
        "PDCP_DATA_REQ/%d Bytes (RRCReconfigurationComplete) "
        "--->][PDCP][RB %02d]\n",
        size,
        srb_id);
  nr_pdcp_data_req_srb(rrc->ue_id, srb_id, 0, size, buffer, deliver_pdu_srb_rlc, NULL);
}

static void nr_rrc_ue_process_rrcReestablishment(NR_UE_RRC_INST_t *rrc,
                                                 const int gNB_index,
                                                 const NR_RRCReestablishment_t *rrcReestablishment,
                                                 int srb_id,
                                                 const uint8_t *msg,
                                                 int msg_size,
                                                 const nr_pdcp_integrity_data_t *msg_integrity)
{
  // implementign procedues as described in 38.331 section 5.3.7.5
  // stop timer T301
  NR_UE_Timers_Constants_t *timers = &rrc->timers_and_constants;
  nr_timer_stop(&timers->T301);
  // store the nextHopChainingCount value
  NR_RRCReestablishment_IEs_t *ies = rrcReestablishment->criticalExtensions.choice.rrcReestablishment;
  AssertFatal(ies, "Not expecting RRCReestablishment_IEs to be NULL\n");
  // TODO need to understand how to use nextHopChainingCount
  // int nh = rrcReestablishment->criticalExtensions.choice.rrcReestablishment->nextHopChainingCount;

  // update the K gNB key based on the current K gNB key or the NH, using the stored nextHopChainingCount value
  nr_derive_key_ng_ran_star(rrc->phyCellID, rrc->arfcn_ssb, rrc->kgnb, rrc->kgnb);

  // derive the K_RRCenc key associated with the previously configured cipheringAlgorithm
  // derive the K_RRCint key associated with the previously configured integrityProtAlgorithm
  nr_pdcp_entity_security_keys_and_algos_t security_parameters;
  security_parameters.ciphering_algorithm = rrc->cipheringAlgorithm;
  security_parameters.integrity_algorithm = rrc->integrityProtAlgorithm;
  nr_derive_key(RRC_ENC_ALG, rrc->cipheringAlgorithm, rrc->kgnb, security_parameters.ciphering_key);
  nr_derive_key(RRC_INT_ALG, rrc->integrityProtAlgorithm, rrc->kgnb, security_parameters.integrity_key);

  // configure lower layers to resume integrity protection for SRB1
  // configure lower layers to resume ciphering for SRB1
  AssertFatal(srb_id == 1, "rrcReestablishment SRB-ID %d, should be 1\n", srb_id);
  nr_pdcp_config_set_security(rrc->ue_id, srb_id, true, &security_parameters);

  // request lower layers to verify the integrity protection of the RRCReestablishment message
  // using the previously configured algorithm and the K_RRCint key
  bool integrity_pass = nr_pdcp_check_integrity_srb(rrc->ue_id, srb_id, msg, msg_size, msg_integrity);
  // if the integrity protection check of the RRCReestablishment message fails
  // perform the actions upon going to RRC_IDLE as specified in 5.3.11
  // with release cause 'RRC connection failure', upon which the procedure ends
  if (!integrity_pass) {
    NR_Release_Cause_t release_cause = RRC_CONNECTION_FAILURE;
    nr_rrc_going_to_IDLE(rrc, release_cause, NULL);
    return;
  }

  // release the measurement gap configuration indicated by the measGapConfig, if configured
  rrcPerNB_t *rrcNB = rrc->perNB + gNB_index;
  asn1cFreeStruc(asn_DEF_NR_MeasGapConfig, rrcNB->measGapConfig);

  // resetting the RA trigger state after receiving MSG4 with RRCReestablishment
  rrc->ra_trigger = RA_NOT_RUNNING;
  // to flag 1st reconfiguration after reestablishment
  rrc->reconfig_after_reestab = true;

  // submit the RRCReestablishmentComplete message to lower layers for transmission
  nr_rrc_ue_generate_rrcReestablishmentComplete(rrc, rrcReestablishment);
}

static int nr_rrc_ue_decode_dcch(NR_UE_RRC_INST_t *rrc,
                                 const srb_id_t Srb_id,
                                 const uint8_t *const Buffer,
                                 size_t Buffer_size,
                                 const uint8_t gNB_indexP,
                                 const nr_pdcp_integrity_data_t *msg_integrity)
{
  NR_DL_DCCH_Message_t *dl_dcch_msg = NULL;
  if (Srb_id != 1 && Srb_id != 2) {
    LOG_E(NR_RRC, "Received message on DL-DCCH (SRB%ld), should not have ...\n", Srb_id);
  }

  LOG_D(NR_RRC, "Decoding DL-DCCH Message\n");
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_DL_DCCH_Message, (void **)&dl_dcch_msg, Buffer, Buffer_size, 0, 0);

  if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
    LOG_E(NR_RRC, "Failed to decode DL-DCCH (%zu bytes)\n", dec_rval.consumed);
    ASN_STRUCT_FREE(asn_DEF_NR_DL_DCCH_Message, dl_dcch_msg);
    return -1;
  }

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_DL_DCCH_Message, (void *)dl_dcch_msg);
  }

  switch (dl_dcch_msg->message.present) {
    case NR_DL_DCCH_MessageType_PR_c1: {
      struct NR_DL_DCCH_MessageType__c1 *c1 = dl_dcch_msg->message.choice.c1;
      switch (c1->present) {
        case NR_DL_DCCH_MessageType__c1_PR_NOTHING:
          LOG_I(NR_RRC, "Received PR_NOTHING on DL-DCCH-Message\n");
          break;

        case NR_DL_DCCH_MessageType__c1_PR_rrcReconfiguration: {
          nr_rrc_ue_process_rrcReconfiguration(rrc, gNB_indexP, c1->choice.rrcReconfiguration);
          if (rrc->reconfig_after_reestab) {
            // if this is the first RRCReconfiguration message after successful completion of the RRC re-establishment procedure
            // resume SRB2 and DRBs that are suspended
            if (rrc->Srb[2] == RB_SUSPENDED)
              rrc->Srb[2] = RB_ESTABLISHED;
            for (int i = 1; i <= MAX_DRBS_PER_UE; i++) {
              if (get_DRB_status(rrc, i) == RB_SUSPENDED)
                set_DRB_status(rrc, i, RB_ESTABLISHED);
            }
            rrc->reconfig_after_reestab = false;
          }
          nr_rrc_ue_generate_RRCReconfigurationComplete(rrc, Srb_id, c1->choice.rrcReconfiguration->rrc_TransactionIdentifier);
        } break;

        case NR_DL_DCCH_MessageType__c1_PR_rrcResume:
          LOG_E(NR_RRC, "Received rrcResume on DL-DCCH-Message -> Not handled\n");
          break;
        case NR_DL_DCCH_MessageType__c1_PR_rrcRelease:
          LOG_I(NR_RRC, "[UE %ld] Received RRC Release (gNB %d)\n", rrc->ue_id, gNB_indexP);
          // delay the actions 60 ms from the moment the RRCRelease message was received
          UPDATE_IE(rrc->RRCRelease, dl_dcch_msg->message.choice.c1->choice.rrcRelease, NR_RRCRelease_t);
          nr_timer_setup(&rrc->release_timer, 60, 10); // 10ms step
          nr_timer_start(&rrc->release_timer);
          break;

        case NR_DL_DCCH_MessageType__c1_PR_ueCapabilityEnquiry:
          LOG_I(NR_RRC, "Received Capability Enquiry (gNB %d)\n", gNB_indexP);
          nr_rrc_ue_process_ueCapabilityEnquiry(rrc, c1->choice.ueCapabilityEnquiry);
          break;

        case NR_DL_DCCH_MessageType__c1_PR_rrcReestablishment:
          LOG_I(NR_RRC, "Logical Channel DL-DCCH (SRB1), Received RRCReestablishment\n");
          nr_rrc_ue_process_rrcReestablishment(rrc,
                                               gNB_indexP,
                                               c1->choice.rrcReestablishment,
                                               Srb_id,
                                               Buffer,
                                               Buffer_size,
                                               msg_integrity);
          break;

        case NR_DL_DCCH_MessageType__c1_PR_dlInformationTransfer: {
          NR_DLInformationTransfer_t *dlInformationTransfer = c1->choice.dlInformationTransfer;

          if (dlInformationTransfer->criticalExtensions.present
              == NR_DLInformationTransfer__criticalExtensions_PR_dlInformationTransfer) {
            /* This message hold a dedicated info NAS payload, forward it to NAS */
            NR_DedicatedNAS_Message_t *dedicatedNAS_Message =
                dlInformationTransfer->criticalExtensions.choice.dlInformationTransfer->dedicatedNAS_Message;

            MessageDef *ittiMsg = itti_alloc_new_message(TASK_RRC_NRUE, rrc->ue_id, NAS_DOWNLINK_DATA_IND);
            dl_info_transfer_ind_t *msg = &NAS_DOWNLINK_DATA_IND(ittiMsg);
            msg->UEid = rrc->ue_id;
            msg->nasMsg.length = dedicatedNAS_Message->size;
            msg->nasMsg.nas_data = malloc(msg->nasMsg.length);
            memcpy(msg->nasMsg.nas_data, dedicatedNAS_Message->buf, msg->nasMsg.length);
            itti_send_msg_to_task(TASK_NAS_NRUE, rrc->ue_id, ittiMsg);
            dedicatedNAS_Message->buf = NULL; // to keep the buffer, up to NAS to free it
          }
        } break;
        case NR_DL_DCCH_MessageType__c1_PR_mobilityFromNRCommand:
        case NR_DL_DCCH_MessageType__c1_PR_dlDedicatedMessageSegment_r16:
        case NR_DL_DCCH_MessageType__c1_PR_ueInformationRequest_r16:
        case NR_DL_DCCH_MessageType__c1_PR_dlInformationTransferMRDC_r16:
        case NR_DL_DCCH_MessageType__c1_PR_loggedMeasurementConfiguration_r16:
        case NR_DL_DCCH_MessageType__c1_PR_spare3:
        case NR_DL_DCCH_MessageType__c1_PR_spare2:
        case NR_DL_DCCH_MessageType__c1_PR_spare1:
        case NR_DL_DCCH_MessageType__c1_PR_counterCheck:
          break;
        case NR_DL_DCCH_MessageType__c1_PR_securityModeCommand:
          LOG_I(NR_RRC, "Received securityModeCommand (gNB %d)\n", gNB_indexP);
          nr_rrc_ue_process_securityModeCommand(rrc, c1->choice.securityModeCommand, Srb_id, Buffer, Buffer_size, msg_integrity);
          break;
      }
    } break;
    default:
      break;
  }
  //  release memory allocation
  SEQUENCE_free(&asn_DEF_NR_DL_DCCH_Message, dl_dcch_msg, ASFM_FREE_EVERYTHING);
  return 0;
}

void nr_rrc_handle_ra_indication(NR_UE_RRC_INST_t *rrc, bool ra_succeeded)
{
  NR_UE_Timers_Constants_t *timers = &rrc->timers_and_constants;
  if (ra_succeeded && nr_timer_is_active(&timers->T304)) {
    // successful Random Access procedure triggered by reconfigurationWithSync
    nr_timer_stop(&timers->T304);
    // TODO handle the rest of procedures as described in 5.3.5.3 for when
    // reconfigurationWithSync is included in spCellConfig
  }
}

void *rrc_nrue_task(void *args_p)
{
  itti_mark_task_ready(TASK_RRC_NRUE);
  while (1) {
    rrc_nrue(NULL);
  }
}

void *rrc_nrue(void *notUsed)
{
  MessageDef *msg_p = NULL;
  itti_receive_msg(TASK_RRC_NRUE, &msg_p);
  instance_t instance = ITTI_MSG_DESTINATION_INSTANCE(msg_p);
  LOG_D(NR_RRC, "[UE %ld] Received %s\n", instance, ITTI_MSG_NAME(msg_p));

  NR_UE_RRC_INST_t *rrc = &NR_UE_rrc_inst[instance];
  AssertFatal(instance == rrc->ue_id, "Instance %ld received from ITTI doesn't matach with UE-ID %ld\n", instance, rrc->ue_id);

  switch (ITTI_MSG_ID(msg_p)) {
  case TERMINATE_MESSAGE:
    LOG_W(NR_RRC, " *** Exiting RRC thread\n");
    itti_exit_task();
    break;

  case MESSAGE_TEST:
    break;

  case NR_RRC_MAC_SYNC_IND: {
    nr_sync_msg_t sync_msg = NR_RRC_MAC_SYNC_IND(msg_p).in_sync ? IN_SYNC : OUT_OF_SYNC;
    NR_UE_Timers_Constants_t *tac = &rrc->timers_and_constants;
    handle_rlf_sync(tac, sync_msg);
  } break;

  case NRRRC_FRAME_PROCESS:
    LOG_D(NR_RRC, "Received %s: frame %d\n", ITTI_MSG_NAME(msg_p), NRRRC_FRAME_PROCESS(msg_p).frame);
    // increase the timers every 10ms (every new frame)
    nr_rrc_handle_timers(rrc);
    NR_UE_RRC_SI_INFO *SInfo = &rrc->perNB[NRRRC_FRAME_PROCESS(msg_p).gnb_id].SInfo;
    nr_rrc_SI_timers(SInfo);
    break;

  case NR_RRC_MAC_INAC_IND:
    LOG_D(NR_RRC, "Received data inactivity indication from lower layers\n");
    NR_Release_Cause_t release_cause = RRC_CONNECTION_FAILURE;
    nr_rrc_going_to_IDLE(rrc, release_cause, NULL);
    break;

  case NR_RRC_RLC_MAXRTX:
    // detection of RLF upon indication from RLC that the maximum number of retransmissions has been reached
    LOG_W(NR_RRC,
          "[UE %ld ID %d] Received indication that RLC reached max retransmissions\n",
          instance,
          NR_RRC_RLC_MAXRTX(msg_p).ue_id);
    handle_rlf_detection(rrc);
    break;

  case NR_RRC_MAC_MSG3_IND:
    nr_rrc_handle_msg3_indication(rrc, NR_RRC_MAC_MSG3_IND(msg_p).rnti);
    break;

  case NR_RRC_MAC_RA_IND:
    LOG_D(NR_RRC,
	  "[UE %ld] Received %s: frame %d RA %s\n",
	  rrc->ue_id,
	  ITTI_MSG_NAME(msg_p),
	  NR_RRC_MAC_RA_IND(msg_p).frame,
	  NR_RRC_MAC_RA_IND(msg_p).RA_succeeded ? "successful" : "failed");
    nr_rrc_handle_ra_indication(rrc, NR_RRC_MAC_RA_IND(msg_p).RA_succeeded);
    break;

  case NR_RRC_MAC_BCCH_DATA_IND:
    LOG_D(NR_RRC, "[UE %ld] Received %s: gNB %d\n", rrc->ue_id, ITTI_MSG_NAME(msg_p), NR_RRC_MAC_BCCH_DATA_IND(msg_p).gnb_index);
    NRRrcMacBcchDataInd *bcch = &NR_RRC_MAC_BCCH_DATA_IND(msg_p);
    if (bcch->is_bch)
      nr_rrc_ue_decode_NR_BCCH_BCH_Message(rrc, bcch->gnb_index, bcch->phycellid, bcch->ssb_arfcn, bcch->sdu, bcch->sdu_size);
    else
      nr_rrc_ue_decode_NR_BCCH_DL_SCH_Message(rrc, bcch->gnb_index, bcch->sdu, bcch->sdu_size, bcch->rsrq, bcch->rsrp);
    break;

  case NR_RRC_MAC_SBCCH_DATA_IND:
    LOG_D(NR_RRC, "[UE %ld] Received %s: gNB %d\n", instance, ITTI_MSG_NAME(msg_p), NR_RRC_MAC_SBCCH_DATA_IND(msg_p).gnb_index);
    NRRrcMacSBcchDataInd *sbcch = &NR_RRC_MAC_SBCCH_DATA_IND(msg_p);

    nr_rrc_ue_decode_NR_SBCCH_SL_BCH_Message(rrc, sbcch->gnb_index,sbcch->frame, sbcch->slot, sbcch->sdu,
                                             sbcch->sdu_size, sbcch->rx_slss_id);
    break;

  case NR_RRC_MAC_CCCH_DATA_IND: {
    NRRrcMacCcchDataInd *ind = &NR_RRC_MAC_CCCH_DATA_IND(msg_p);
    nr_rrc_ue_decode_ccch(rrc, ind);
  } break;

  case NR_RRC_DCCH_DATA_IND:
    nr_rrc_ue_decode_dcch(rrc,
			  NR_RRC_DCCH_DATA_IND(msg_p).dcch_index,
			  NR_RRC_DCCH_DATA_IND(msg_p).sdu_p,
			  NR_RRC_DCCH_DATA_IND(msg_p).sdu_size,
			  NR_RRC_DCCH_DATA_IND(msg_p).gNB_index,
			  &NR_RRC_DCCH_DATA_IND(msg_p).msg_integrity);
    /* this is allocated by itti_malloc in PDCP task (deliver_sdu_srb)
       then passed to the RRC task and freed after use */
    free(NR_RRC_DCCH_DATA_IND(msg_p).sdu_p);
    break;

  case NAS_KENB_REFRESH_REQ:
    memcpy(rrc->kgnb, NAS_KENB_REFRESH_REQ(msg_p).kenb, sizeof(rrc->kgnb));
    break;

  case NAS_DETACH_REQ:
    if (NAS_DETACH_REQ(msg_p).wait_release)
      rrc->detach_after_release = true;
    else {
      rrc->nrRrcState = RRC_STATE_DETACH_NR;
      NR_Release_Cause_t release_cause = OTHER;
      nr_rrc_going_to_IDLE(rrc, release_cause, NULL);
    }
    break;

  case NAS_UPLINK_DATA_REQ: {
    uint32_t length;
    uint8_t *buffer = NULL;
    ul_info_transfer_req_t *req = &NAS_UPLINK_DATA_REQ(msg_p);
    /* Create message for PDCP (ULInformationTransfer_t) */
    length = do_NR_ULInformationTransfer(&buffer, req->nasMsg.length, req->nasMsg.nas_data);
    /* Transfer data to PDCP */
    // check if SRB2 is created, if yes request data_req on SRB2
    // error: the remote gNB is hardcoded here
    rb_id_t srb_id = rrc->Srb[2] == RB_ESTABLISHED ? 2 : 1;
    nr_pdcp_data_req_srb(rrc->ue_id, srb_id, 0, length, buffer, deliver_pdu_srb_rlc, NULL);
    free(req->nasMsg.nas_data);
    free(buffer);
    break;
  }

  case NAS_5GMM_IND: {
    nas_5gmm_ind_t *req = &NAS_5GMM_IND(msg_p);
    rrc->fiveG_S_TMSI = req->fiveG_STMSI;
    break;
  }

  default:
    LOG_E(NR_RRC, "[UE %ld] Received unexpected message %s\n", rrc->ue_id, ITTI_MSG_NAME(msg_p));
    break;
  }
  LOG_D(NR_RRC, "[UE %ld] RRC Status %d\n", rrc->ue_id, rrc->nrRrcState);
  int result = itti_free(ITTI_MSG_ORIGIN_ID(msg_p), msg_p);
  AssertFatal(result == EXIT_SUCCESS, "Failed to free memory (%d)!\n", result);
  return NULL;
}

void nr_rrc_ue_process_sidelink_radioResourceConfig(NR_SetupRelease_SL_ConfigDedicatedNR_r16_t *sl_ConfigDedicatedNR)
{
  //process sl_CommConfig, configure MAC/PHY for transmitting SL communication (RRC_CONNECTED)
  if (sl_ConfigDedicatedNR != NULL) {
    switch (sl_ConfigDedicatedNR->present){
      case NR_SetupRelease_SL_ConfigDedicatedNR_r16_PR_setup:
        //TODO
        break;
      case NR_SetupRelease_SL_ConfigDedicatedNR_r16_PR_release:
        break;
      case NR_SetupRelease_SL_ConfigDedicatedNR_r16_PR_NOTHING:
        break;
      default:
        break;
    }
  }
}

static void nr_rrc_ue_process_ueCapabilityEnquiry(NR_UE_RRC_INST_t *rrc, NR_UECapabilityEnquiry_t *UECapabilityEnquiry)
{
  NR_UL_DCCH_Message_t ul_dcch_msg = {0};
  //
  LOG_I(NR_RRC, "Receiving from SRB1 (DL-DCCH), Processing UECapabilityEnquiry\n");

  ul_dcch_msg.message.present = NR_UL_DCCH_MessageType_PR_c1;
  asn1cCalloc(ul_dcch_msg.message.choice.c1, c1);
  c1->present = NR_UL_DCCH_MessageType__c1_PR_ueCapabilityInformation;
  asn1cCalloc(c1->choice.ueCapabilityInformation, info);
  info->rrc_TransactionIdentifier = UECapabilityEnquiry->rrc_TransactionIdentifier;
  if (!rrc->UECap.UE_NR_Capability) {
    rrc->UECap.UE_NR_Capability = CALLOC(1, sizeof(NR_UE_NR_Capability_t));
    asn1cSequenceAdd(rrc->UECap.UE_NR_Capability->rf_Parameters.supportedBandListNR.list, NR_BandNR_t, nr_bandnr);
    nr_bandnr->bandNR = 1;
  }
  xer_fprint(stdout, &asn_DEF_NR_UE_NR_Capability, (void *)rrc->UECap.UE_NR_Capability);

  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_UE_NR_Capability,
                                                  NULL,
                                                  (void *)rrc->UECap.UE_NR_Capability,
                                                  &rrc->UECap.sdu[0],
                                                  MAX_UE_NR_CAPABILITY_SIZE);
  AssertFatal (enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %lu)!\n",
               enc_rval.failed_type->name, enc_rval.encoded);
  rrc->UECap.sdu_size = (enc_rval.encoded + 7) / 8;
  LOG_I(PHY, "[RRC]UE NR Capability encoded, %d bytes (%zd bits)\n", rrc->UECap.sdu_size, enc_rval.encoded + 7);
  /* RAT Container */
  NR_UE_CapabilityRAT_Container_t *ue_CapabilityRAT_Container = CALLOC(1, sizeof(NR_UE_CapabilityRAT_Container_t));
  ue_CapabilityRAT_Container->rat_Type = NR_RAT_Type_nr;
  OCTET_STRING_fromBuf(&ue_CapabilityRAT_Container->ue_CapabilityRAT_Container, (const char *)rrc->UECap.sdu, rrc->UECap.sdu_size);
  NR_UECapabilityEnquiry_IEs_t *ueCapabilityEnquiry_ie = UECapabilityEnquiry->criticalExtensions.choice.ueCapabilityEnquiry;
  if (get_softmodem_params()->nsa == 1) {
    OCTET_STRING_t *requestedFreqBandsNR = ueCapabilityEnquiry_ie->ue_CapabilityEnquiryExt;
    nsa_sendmsg_to_lte_ue(requestedFreqBandsNR->buf, requestedFreqBandsNR->size, UE_CAPABILITY_INFO);
  }
  //  ue_CapabilityRAT_Container.ueCapabilityRAT_Container.buf  = UE_rrc_inst[ue_mod_idP].UECapability;
  // ue_CapabilityRAT_Container.ueCapabilityRAT_Container.size = UE_rrc_inst[ue_mod_idP].UECapability_size;
  AssertFatal(UECapabilityEnquiry->criticalExtensions.present == NR_UECapabilityEnquiry__criticalExtensions_PR_ueCapabilityEnquiry,
              "UECapabilityEnquiry->criticalExtensions.present (%d) != UECapabilityEnquiry__criticalExtensions_PR_c1 (%d)\n",
              UECapabilityEnquiry->criticalExtensions.present,NR_UECapabilityEnquiry__criticalExtensions_PR_ueCapabilityEnquiry);

  NR_UECapabilityInformation_t *ueCapabilityInformation = ul_dcch_msg.message.choice.c1->choice.ueCapabilityInformation;
  ueCapabilityInformation->criticalExtensions.present = NR_UECapabilityInformation__criticalExtensions_PR_ueCapabilityInformation;
  asn1cCalloc(ueCapabilityInformation->criticalExtensions.choice.ueCapabilityInformation, infoIE);
  asn1cCalloc(infoIE->ue_CapabilityRAT_ContainerList, UEcapList);
  UEcapList->list.count = 0;

  for (int i = 0; i < ueCapabilityEnquiry_ie->ue_CapabilityRAT_RequestList.list.count; i++) {
    if (ueCapabilityEnquiry_ie->ue_CapabilityRAT_RequestList.list.array[i]->rat_Type == NR_RAT_Type_nr) {
      asn1cSeqAdd(&UEcapList->list, ue_CapabilityRAT_Container);
      uint8_t buffer[500];
      asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_UL_DCCH_Message, NULL, (void *)&ul_dcch_msg, buffer, 500);
      AssertFatal (enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %jd)!\n",
                   enc_rval.failed_type->name, enc_rval.encoded);

      if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
        xer_fprint(stdout, &asn_DEF_NR_UL_DCCH_Message, (void *)&ul_dcch_msg);
      }
      LOG_I(NR_RRC, "UECapabilityInformation Encoded %zd bits (%zd bytes)\n",enc_rval.encoded,(enc_rval.encoded+7)/8);
      int srb_id = 1; // UECapabilityInformation on SRB1
      nr_pdcp_data_req_srb(rrc->ue_id, srb_id, 0, (enc_rval.encoded + 7) / 8, buffer, deliver_pdu_srb_rlc, NULL);
    }
  }
  /* Free struct members after it's done
     including locally allocated ue_CapabilityRAT_Container */
  ASN_STRUCT_RESET(asn_DEF_NR_UL_DCCH_Message, &ul_dcch_msg);
}

static void nr_rrc_initiate_rrcReestablishment(NR_UE_RRC_INST_t *rrc, NR_ReestablishmentCause_t cause)
{
  rrc->reestablishment_cause = cause;

  NR_UE_Timers_Constants_t *timers = &rrc->timers_and_constants;

  // reset timers to SIB1 as part of release of spCellConfig
  // it needs to be done before handling timers
  set_rlf_sib1_timers_and_constants(timers, rrc->timers_and_constants.sib1_TimersAndConstants);

  // stop timer T310, if running
  nr_timer_stop(&timers->T310);
  // stop timer T304, if running
  nr_timer_stop(&timers->T304);
  // start timer T311
  nr_timer_start(&timers->T311);
  // suspend all RBs, except SRB0
  for (int i = 1; i < 4; i++) {
    if (rrc->Srb[i] == RB_ESTABLISHED) {
      rrc->Srb[i] = RB_SUSPENDED;
    }
  }
  for (int i = 1; i <= MAX_DRBS_PER_UE; i++) {
    if (get_DRB_status(rrc, i) == RB_ESTABLISHED) {
      set_DRB_status(rrc, i, RB_SUSPENDED);
    }
  }
  // release the MCG SCell(s), if configured
  // no SCell configured in our implementation

  // reset MAC
  // release spCellConfig, if configured
  // perform cell selection in accordance with the cell selection process
  MessageDef *msg = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_MAC_RRC_CONFIG_RESET);
  NR_MAC_RRC_CONFIG_RESET(msg).cause = RE_ESTABLISHMENT;
  itti_send_msg_to_task(TASK_MAC_UE, rrc->ue_id, msg);
}

static void nr_rrc_ue_generate_rrcReestablishmentComplete(const NR_UE_RRC_INST_t *rrc,
                                                          const NR_RRCReestablishment_t *rrcReestablishment)
{
  uint8_t buffer[RRC_BUFFER_SIZE] = {0};
  int size = do_RRCReestablishmentComplete(buffer, RRC_BUFFER_SIZE,
                                           rrcReestablishment->rrc_TransactionIdentifier);
  LOG_I(NR_RRC, "[RAPROC] Logical Channel UL-DCCH (SRB1), Generating RRCReestablishmentComplete (bytes %d)\n", size);
  int srb_id = 1; // RRC re-establishment complete on SRB1
  nr_pdcp_data_req_srb(rrc->ue_id, srb_id, 0, size, buffer, deliver_pdu_srb_rlc, NULL);
}

void *recv_msgs_from_lte_ue(void *args_p)
{
  itti_mark_task_ready (TASK_RRC_NSA_NRUE);
  int from_lte_ue_fd = get_from_lte_ue_fd();
  for (;;) {
    nsa_msg_t msg;
    int recvLen = recvfrom(from_lte_ue_fd, &msg, sizeof(msg), MSG_WAITALL | MSG_TRUNC, NULL, NULL);
    if (recvLen == -1) {
      LOG_E(NR_RRC, "%s: recvfrom: %s\n", __func__, strerror(errno));
      continue;
    }
    if (recvLen > sizeof(msg)) {
      LOG_E(NR_RRC, "%s: Received truncated message %d\n", __func__, recvLen);
      continue;
    }
    process_lte_nsa_msg(NR_UE_rrc_inst, &msg, recvLen);
  }
  return NULL;
}

static void nsa_rrc_ue_process_ueCapabilityEnquiry(NR_UE_RRC_INST_t *rrc)
{
  NR_UE_NR_Capability_t *UE_Capability_nr = rrc->UECap.UE_NR_Capability = CALLOC(1, sizeof(NR_UE_NR_Capability_t));
  NR_BandNR_t *nr_bandnr = CALLOC(1, sizeof(NR_BandNR_t));
  nr_bandnr->bandNR = 78;
  asn1cSeqAdd(&UE_Capability_nr->rf_Parameters.supportedBandListNR.list, nr_bandnr);
  OAI_NR_UECapability_t *UECap = CALLOC(1, sizeof(OAI_NR_UECapability_t));
  UECap->UE_NR_Capability = UE_Capability_nr;

  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_UE_NR_Capability,
                                   NULL,
                                   (void *)UE_Capability_nr,
                                   &UECap->sdu[0],
                                   MAX_UE_NR_CAPABILITY_SIZE);
  AssertFatal (enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %lu)!\n",
               enc_rval.failed_type->name, enc_rval.encoded);
  UECap->sdu_size = (enc_rval.encoded + 7) / 8;
  LOG_A(NR_RRC, "[NR_RRC] NRUE Capability encoded, %d bytes (%zd bits)\n",
        UECap->sdu_size, enc_rval.encoded + 7);

  NR_UE_CapabilityRAT_Container_t ue_CapabilityRAT_Container;
  memset(&ue_CapabilityRAT_Container, 0, sizeof(NR_UE_CapabilityRAT_Container_t));
  ue_CapabilityRAT_Container.rat_Type = NR_RAT_Type_nr;
  OCTET_STRING_fromBuf(&ue_CapabilityRAT_Container.ue_CapabilityRAT_Container,
                       (const char *)rrc->UECap.sdu,
                       rrc->UECap.sdu_size);

  nsa_sendmsg_to_lte_ue(ue_CapabilityRAT_Container.ue_CapabilityRAT_Container.buf,
                        ue_CapabilityRAT_Container.ue_CapabilityRAT_Container.size,
                        NRUE_CAPABILITY_INFO);
}

static void process_lte_nsa_msg(NR_UE_RRC_INST_t *rrc, nsa_msg_t *msg, int msg_len)
{
  if (msg_len < sizeof(msg->msg_type)) {
    LOG_E(RRC, "Msg_len = %d\n", msg_len);
    return;
  }
  LOG_D(NR_RRC, "Processing an NSA message\n");
  Rrc_Msg_Type_t msg_type = msg->msg_type;
  uint8_t *const msg_buffer = msg->msg_buffer;
  msg_len -= sizeof(msg->msg_type);
  switch (msg_type) {
    case UE_CAPABILITY_ENQUIRY: {
      LOG_D(NR_RRC, "We are processing a %d message \n", msg_type);
      NR_FreqBandList_t *nr_freq_band_list = NULL;
      asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                     &asn_DEF_NR_FreqBandList,
                                                     (void **)&nr_freq_band_list,
                                                     msg_buffer,
                                                     msg_len);
      if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
        SEQUENCE_free(&asn_DEF_NR_FreqBandList, nr_freq_band_list, ASFM_FREE_EVERYTHING);
        LOG_E(RRC, "Failed to decode UECapabilityInfo (%zu bits)\n", dec_rval.consumed);
        break;
      }
      for (int i = 0; i < nr_freq_band_list->list.count; i++) {
        LOG_D(NR_RRC, "Received NR band information: %ld.\n",
        nr_freq_band_list->list.array[i]->choice.bandInformationNR->bandNR);
      }
      int dummy_msg = 0;// whatever piece of data, it will never be used by sendee
      LOG_D(NR_RRC, "We are calling nsa_sendmsg_to_lte_ue to send a UE_CAPABILITY_DUMMY\n");
      nsa_sendmsg_to_lte_ue(&dummy_msg, sizeof(dummy_msg), UE_CAPABILITY_DUMMY);
      LOG_A(NR_RRC, "Sent initial NRUE Capability response to LTE UE\n");
      break;
    }

    case NRUE_CAPABILITY_ENQUIRY: {
      LOG_I(NR_RRC, "We are processing a %d message \n", msg_type);
      NR_FreqBandList_t *nr_freq_band_list = NULL;
      asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                     &asn_DEF_NR_FreqBandList,
                                                     (void **)&nr_freq_band_list,
                                                     msg_buffer,
                                                     msg_len);
      if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
        SEQUENCE_free(&asn_DEF_NR_FreqBandList, nr_freq_band_list, ASFM_FREE_EVERYTHING);
        LOG_E(NR_RRC, "Failed to decode UECapabilityInfo (%zu bits)\n", dec_rval.consumed);
        break;
      }
      LOG_I(NR_RRC, "Calling nsa_rrc_ue_process_ueCapabilityEnquiry\n");
      nsa_rrc_ue_process_ueCapabilityEnquiry(rrc);
      break;
    }

    case RRC_MEASUREMENT_PROCEDURE: {
      LOG_I(NR_RRC, "We are processing a %d message \n", msg_type);

      LTE_MeasObjectToAddMod_t *nr_meas_obj = NULL;
      asn_dec_rval_t dec_rval = uper_decode_complete(NULL,
                                                     &asn_DEF_NR_MeasObjectToAddMod,
                                                     (void **)&nr_meas_obj,
                                                     msg_buffer,
                                                     msg_len);
      if ((dec_rval.code != RC_OK) && (dec_rval.consumed == 0)) {
        SEQUENCE_free(&asn_DEF_NR_MeasObjectToAddMod, nr_meas_obj, ASFM_FREE_EVERYTHING);
        LOG_E(RRC, "Failed to decode measurement object (%zu bits) %d\n", dec_rval.consumed, dec_rval.code);
        break;
      }
      LOG_D(NR_RRC, "NR carrierFreq_r15 (ssb): %ld and sub carrier spacing:%ld\n",
            nr_meas_obj->measObject.choice.measObjectNR_r15.carrierFreq_r15,
            nr_meas_obj->measObject.choice.measObjectNR_r15.rs_ConfigSSB_r15.subcarrierSpacingSSB_r15);
      start_oai_nrue_threads();
      break;
    }

    case RRC_CONFIG_COMPLETE_REQ: {
      struct msg {
        uint32_t RadioBearer_size;
        uint32_t SecondaryCellGroup_size;
        uint8_t trans_id;
        uint8_t padding[3];
        uint8_t buffer[];
      } hdr;
      AssertFatal(msg_len >= sizeof(hdr), "Bad received msg\n");
      memcpy(&hdr, msg_buffer, sizeof(hdr));
      LOG_I(NR_RRC, "We got an RRC_CONFIG_COMPLETE_REQ\n");
      uint32_t nr_RadioBearer_size = hdr.RadioBearer_size;
      uint32_t nr_SecondaryCellGroup_size = hdr.SecondaryCellGroup_size;
      AssertFatal(sizeof(hdr) + nr_RadioBearer_size + nr_SecondaryCellGroup_size <= msg_len,
                  "nr_RadioBearerConfig1_r15 size %u nr_SecondaryCellGroupConfig_r15 size %u sizeof(hdr) %zu, msg_len = %d\n",
                  nr_RadioBearer_size,
                  nr_SecondaryCellGroup_size,
                  sizeof(hdr),
                  msg_len);
      NR_RRC_TransactionIdentifier_t t_id = hdr.trans_id;
      LOG_I(NR_RRC, "nr_RadioBearerConfig1_r15 size %d nr_SecondaryCellGroupConfig_r15 size %d t_id %ld\n",
            nr_RadioBearer_size,
            nr_SecondaryCellGroup_size,
            t_id);

      uint8_t *nr_RadioBearer_buffer = msg_buffer + offsetof(struct msg, buffer);
      uint8_t *nr_SecondaryCellGroup_buffer = nr_RadioBearer_buffer + nr_RadioBearer_size;
      process_nsa_message(NR_UE_rrc_inst, nr_SecondaryCellGroupConfig_r15, nr_SecondaryCellGroup_buffer, nr_SecondaryCellGroup_size);
      process_nsa_message(NR_UE_rrc_inst, nr_RadioBearerConfigX_r15, nr_RadioBearer_buffer, nr_RadioBearer_size);
      LOG_I(NR_RRC, "Calling do_NR_RRCReconfigurationComplete. t_id %ld \n", t_id);
      uint8_t buffer[NR_RRC_BUF_SIZE];
      size_t size = do_NR_RRCReconfigurationComplete_for_nsa(buffer, sizeof(buffer), t_id);
      nsa_sendmsg_to_lte_ue(buffer, size, NR_RRC_CONFIG_COMPLETE_REQ);
      break;
    }

    case OAI_TUN_IFACE_NSA: {
      LOG_I(NR_RRC, "We got an OAI_TUN_IFACE_NSA!!\n");
      char cmd_line[NR_RRC_BUF_SIZE];
      memcpy(cmd_line, msg_buffer, sizeof(cmd_line));
      LOG_D(NR_RRC, "Command line: %s\n", cmd_line);
      if (background_system(cmd_line) != 0)
        LOG_E(NR_RRC, "ESM-PROC - failed command '%s'", cmd_line);
      break;
    }

    default:
      LOG_E(NR_RRC, "No NSA Message Found\n");
  }
}

void handle_RRCRelease(NR_UE_RRC_INST_t *rrc)
{
  NR_UE_Timers_Constants_t *tac = &rrc->timers_and_constants;
  // stop timer T380, if running
  nr_timer_stop(&tac->T380);
  // stop timer T320, if running
  nr_timer_stop(&tac->T320);
  if (rrc->detach_after_release)
    rrc->nrRrcState = RRC_STATE_DETACH_NR;
  const struct NR_RRCRelease_IEs *rrcReleaseIEs = rrc->RRCRelease ? rrc->RRCRelease->criticalExtensions.choice.rrcRelease : NULL;
  if (!rrc->as_security_activated) {
    // ignore any field included in RRCRelease message except waitTime
    // perform the actions upon going to RRC_IDLE as specified in 5.3.11 with the release cause 'other'
    // upon which the procedure ends
    NR_Release_Cause_t cause = OTHER;
    nr_rrc_going_to_IDLE(rrc, cause, rrc->RRCRelease);
    asn1cFreeStruc(asn_DEF_NR_RRCRelease, rrc->RRCRelease);
    return;
  }
  bool suspend = false;
  if (rrcReleaseIEs) {
    if (rrcReleaseIEs->redirectedCarrierInfo)
      LOG_E(NR_RRC, "redirectedCarrierInfo in RRCRelease not handled\n");
    if (rrcReleaseIEs->cellReselectionPriorities)
      LOG_E(NR_RRC, "cellReselectionPriorities in RRCRelease not handled\n");
    if (rrcReleaseIEs->deprioritisationReq)
      LOG_E(NR_RRC, "deprioritisationReq in RRCRelease not handled\n");
    if (rrcReleaseIEs->suspendConfig) {
      suspend = true;
      // procedures to go in INACTIVE state
      AssertFatal(false, "Inactive State not supported\n");
    }
  }
  if (!suspend) {
    NR_Release_Cause_t cause = OTHER;
    nr_rrc_going_to_IDLE(rrc, cause, rrc->RRCRelease);
  }
  asn1cFreeStruc(asn_DEF_NR_RRCRelease, rrc->RRCRelease);
}

void handle_rlf_detection(NR_UE_RRC_INST_t *rrc)
{
  // 5.3.10.3 in 38.331
  bool srb2 = rrc->Srb[2] != RB_NOT_PRESENT;
  bool any_drb = false;
  for (int i = 0; i < MAX_DRBS_PER_UE; i++) {
    if (rrc->status_DRBs[i] != RB_NOT_PRESENT) {
      any_drb = true;
      break;
    }
  }

  if (rrc->as_security_activated && srb2 && any_drb) // initiate the connection re-establishment procedure
    nr_rrc_initiate_rrcReestablishment(rrc, NR_ReestablishmentCause_otherFailure);
  else {
    NR_Release_Cause_t cause = rrc->as_security_activated ? RRC_CONNECTION_FAILURE : OTHER;
    nr_rrc_going_to_IDLE(rrc, cause, NULL);
  }
}

void nr_rrc_going_to_IDLE(NR_UE_RRC_INST_t *rrc,
                          NR_Release_Cause_t release_cause,
                          NR_RRCRelease_t *RRCRelease)
{
  NR_UE_Timers_Constants_t *tac = &rrc->timers_and_constants;

  // if going to RRC_IDLE was triggered by reception
  // of the RRCRelease message including a waitTime
  NR_RejectWaitTime_t *waitTime = NULL;
  if (RRCRelease) {
    struct NR_RRCRelease_IEs *rrcReleaseIEs = RRCRelease->criticalExtensions.choice.rrcRelease;
    if(rrcReleaseIEs) {
      waitTime = rrcReleaseIEs->nonCriticalExtension ?
                 rrcReleaseIEs->nonCriticalExtension->waitTime : NULL;
      if (waitTime) {
        nr_timer_stop(&tac->T302); // stop 302
        // start timer T302 with the value set to the waitTime
        int target = *waitTime * 1000; // waitTime is in seconds
        nr_timer_setup(&tac->T302, target, 10);
        nr_timer_start(&tac->T302);
        // TODO inform upper layers that access barring is applicable
        // for all access categories except categories '0' and '2'.
        LOG_E(NR_RRC,"Go to IDLE. Handling RRCRelease message including a waitTime not implemented\n");
      }
    }
  }
  if (!waitTime) {
    if (nr_timer_is_active(&tac->T302)) {
      nr_timer_stop(&tac->T302);
      // TODO barring alleviation as in 5.3.14.4
      // not implemented
      LOG_E(NR_RRC,"Go to IDLE. Barring alleviation not implemented\n");
    }
  }
  if (nr_timer_is_active(&tac->T390)) {
    nr_timer_stop(&tac->T390);
    // TODO barring alleviation as in 5.3.14.4
    // not implemented
    LOG_E(NR_RRC,"Go to IDLE. Barring alleviation not implemented\n");
  }
  if (!RRCRelease && rrc->nrRrcState == RRC_STATE_INACTIVE_NR) {
    // TODO discard the cell reselection priority information provided by the cellReselectionPriorities
    // cell reselection priorities not implemented yet
    nr_timer_stop(&tac->T320);
  }
  // Stop all the timers except T302, T320 and T325
  nr_timer_stop(&tac->T300);
  nr_timer_stop(&tac->T301);
  nr_timer_stop(&tac->T304);
  nr_timer_stop(&tac->T310);
  nr_timer_stop(&tac->T311);
  nr_timer_stop(&tac->T319);

  // discard the UE Inactive AS context
  // TODO there is no inactive AS context

  // release the suspendConfig
  // TODO suspendConfig not handled yet

  // discard the keys (only kgnb is stored)
  memset(rrc->kgnb, 0, sizeof(rrc->kgnb));
  rrc->integrityProtAlgorithm = 0;
  rrc->cipheringAlgorithm = 0;

  // release all radio resources, including release of the RLC entity,
  // the MAC configuration and the associated PDCP entity
  // and SDAP for all established RBs
  for (int i = 1; i <= MAX_DRBS_PER_UE; i++) {
    if (get_DRB_status(rrc, i) != RB_NOT_PRESENT) {
      set_DRB_status(rrc, i, RB_NOT_PRESENT);
      nr_pdcp_release_drb(rrc->ue_id, i);
    }
  }
  for (int i = 1; i < NR_NUM_SRB; i++) {
    if (rrc->Srb[i] != RB_NOT_PRESENT) {
      rrc->Srb[i] = RB_NOT_PRESENT;
      nr_pdcp_release_srb(rrc->ue_id, i);
    }
  }
  for (int i = 0; i < NR_MAX_NUM_LCID; i++) {
    if (rrc->active_RLC_entity[i]) {
      rrc->active_RLC_entity[i] = false;
      nr_rlc_release_entity(rrc->ue_id, i);
    }
  }

  for (int i = 0; i < NB_CNX_UE; i++) {
    rrcPerNB_t *nb = &rrc->perNB[i];
    NR_UE_RRC_SI_INFO *SI_info = &nb->SInfo;
    init_SI_timers(SI_info);
    SI_info->sib1_validity = SIB_NOT_VALID;
    SI_info->sib2_validity = SIB_NOT_VALID;
    SI_info->sib3_validity = SIB_NOT_VALID;
    SI_info->sib4_validity = SIB_NOT_VALID;
    SI_info->sib5_validity = SIB_NOT_VALID;
    SI_info->sib6_validity = SIB_NOT_VALID;
    SI_info->sib7_validity = SIB_NOT_VALID;
    SI_info->sib8_validity = SIB_NOT_VALID;
    SI_info->sib9_validity = SIB_NOT_VALID;
    SI_info->sib10_validity = SIB_NOT_VALID;
    SI_info->sib11_validity = SIB_NOT_VALID;
    SI_info->sib12_validity = SIB_NOT_VALID;
    SI_info->sib13_validity = SIB_NOT_VALID;
    SI_info->sib14_validity = SIB_NOT_VALID;
    SI_info->SInfo_r17.sib15_validity = SIB_NOT_VALID;
    SI_info->SInfo_r17.sib16_validity = SIB_NOT_VALID;
    SI_info->SInfo_r17.sib17_validity = SIB_NOT_VALID;
    SI_info->SInfo_r17.sib18_validity = SIB_NOT_VALID;
    SI_info->SInfo_r17.sib19_validity = SIB_NOT_VALID;
    SI_info->SInfo_r17.sib20_validity = SIB_NOT_VALID;
    SI_info->SInfo_r17.sib21_validity = SIB_NOT_VALID;
  }

  if (rrc->nrRrcState == RRC_STATE_DETACH_NR) {
    asn1cFreeStruc(asn_DEF_NR_UE_NR_Capability, rrc->UECap.UE_NR_Capability);
    asn1cFreeStruc(asn_DEF_NR_UE_TimersAndConstants, tac->sib1_TimersAndConstants);
  }

  // reset MAC
  NR_UE_MAC_reset_cause_t cause = (rrc->nrRrcState == RRC_STATE_DETACH_NR) ? DETACH : GO_TO_IDLE;
  MessageDef *msg = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_MAC_RRC_CONFIG_RESET);
  NR_MAC_RRC_CONFIG_RESET(msg).cause = cause;
  itti_send_msg_to_task(TASK_MAC_UE, rrc->ue_id, msg);

  // enter RRC_IDLE
  LOG_I(NR_RRC, "RRC moved into IDLE state\n");
  if (rrc->nrRrcState != RRC_STATE_DETACH_NR)
    rrc->nrRrcState = RRC_STATE_IDLE_NR;

  rrc->rnti = 0;

  // Indicate the release of the RRC connection to upper layers
  MessageDef *msg_p = itti_alloc_new_message(TASK_RRC_NRUE, rrc->ue_id, NR_NAS_CONN_RELEASE_IND);
  NR_NAS_CONN_RELEASE_IND(msg_p).cause = release_cause;
  itti_send_msg_to_task(TASK_NAS_NRUE, rrc->ue_id, msg_p);
}

void handle_t300_expiry(NR_UE_RRC_INST_t *rrc)
{
  rrc->ra_trigger = RRC_CONNECTION_SETUP;
  nr_rrc_ue_prepare_RRCSetupRequest(rrc);

  // reset MAC, release the MAC configuration
  NR_UE_MAC_reset_cause_t cause = T300_EXPIRY;
  MessageDef *msg = itti_alloc_new_message(TASK_RRC_NRUE, 0, NR_MAC_RRC_CONFIG_RESET);
  NR_MAC_RRC_CONFIG_RESET(msg).cause = cause;
  itti_send_msg_to_task(TASK_MAC_UE, rrc->ue_id, msg);
  // TODO handle connEstFailureControl
  // TODO inform upper layers about the failure to establish the RRC connection
}

//This calls the sidelink preconf message after RRC, MAC instances are created.
void start_sidelink(int instance)
{

  NR_UE_RRC_INST_t *rrc = &NR_UE_rrc_inst[instance];

  if (get_softmodem_params()->sl_mode == 2) {

    //Process the Sidelink Preconfiguration
    rrc_ue_process_sidelink_Preconfiguration(rrc, get_softmodem_params()->sync_ref);

  }
}
