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

/*! \file gNB_scheduler_ulsch.c
 * \brief gNB procedures for the ULSCH transport channel
 * \author Navid Nikaein and Raymond Knopp, Guido Casati
 * \date 2019
 * \email: guido.casati@iis.fraunhofer.de
 * \version 1.0
 * @ingroup _mac
 */


#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "executables/softmodem-common.h"
#include "common/utils/nr/nr_common.h"
#include "utils.h"
#include <openair2/UTIL/OPT/opt.h>
#include "LAYER2/NR_MAC_COMMON/nr_mac_extern.h"
#include "LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "LAYER2/RLC/rlc.h"

//#define SRS_IND_DEBUG

int get_ul_tda(gNB_MAC_INST *nrmac, int frame, int slot)
{
  /* we assume that this function is mutex-protected from outside */
  NR_SCHED_ENSURE_LOCKED(&nrmac->sched_lock);

  /* there is a mixed slot only when in TDD */
  frame_structure_t *fs = &nrmac->frame_structure;

  if (fs->frame_type == TDD) {
    // if there is uplink symbols in mixed slot
    int s = get_slot_idx_in_period(slot, fs);
    tdd_bitmap_t *tdd_slot_bitmap = fs->period_cfg.tdd_slot_bitmap;
    if ((tdd_slot_bitmap[s].num_ul_symbols > 1) && is_mixed_slot(s, fs)) {
      return 2;
    }
  }

  // Avoid slots with the SRS
  UE_iterator(nrmac->UE_info.list, UE) {
    NR_sched_srs_t sched_srs = UE->UE_sched_ctrl.sched_srs;
    if(sched_srs.srs_scheduled && sched_srs.frame == frame && sched_srs.slot == slot)
      return 1;
  }

  return 0; // if FDD or not mixed slot in TDD, for now use default TDA (TODO handle CSI-RS slots)
}

static int compute_ph_factor(int mu, int tbs_bits, int rb, int n_layers, int n_symbols, int n_dmrs, long *deltaMCS, bool include_bw)
{
  // 38.213 7.1.1
  // if the PUSCH transmission is over more than one layer delta_tf = 0
  float delta_tf = 0;
  if(deltaMCS != NULL && n_layers == 1) {
    const int n_re = (NR_NB_SC_PER_RB * n_symbols - n_dmrs) * rb;
    const float BPRE = (float) tbs_bits/n_re;  //TODO change for PUSCH with CSI
    const float f = pow(2, BPRE * 1.25);
    const float beta = 1.0f; //TODO change for PUSCH with CSI
    delta_tf = (10 * log10((f - 1) * beta));
    LOG_D(NR_MAC,
          "PH factor delta_tf %f (n_re %d, n_rb %d, n_dmrs %d, n_symbols %d, tbs %d BPRE %f f %f)\n",
          delta_tf,
          n_re,
          rb,
          n_dmrs,
          n_symbols,
          tbs_bits,
          BPRE,
          f);
  }
  const float bw_factor = (include_bw) ? 10 * log10(rb << mu) : 0;
  return ((int)roundf(delta_tf + bw_factor));
}

/* \brief over-estimate the BSR index, given real_index.
 *
 * BSR does not account for headers, so we need to estimate. See 38.321
 * 6.1.3.1: "The size of the RLC headers and MAC subheaders are not considered
 * in the buffer size computation." */
static int overestim_bsr_index(int real_index)
{
  /* if UE reports BSR 0, it means "no data"; otherwise, overestimate to
   * account for headers */
  const int add_overestim = 1;
  return real_index > 0 ? real_index + add_overestim : real_index;
}

static int estimate_ul_buffer_short_bsr(const NR_BSR_SHORT *bsr)
{
  /* NOTE: the short BSR might be for different LCGID than 0, but we do not
   * differentiate them */
  int rep_idx = bsr->Buffer_size;
  int estim_idx = overestim_bsr_index(rep_idx);
  int max = sizeofArray(NR_SHORT_BSR_TABLE) - 1;
  int idx = min(estim_idx, max);
  int estim_size = NR_SHORT_BSR_TABLE[idx];
  LOG_D(NR_MAC, "short BSR LCGID %d index %d estim index %d size %d\n", bsr->LcgID, rep_idx, estim_idx, estim_size);
  return estim_size;
}

static int estimate_ul_buffer_long_bsr(const NR_BSR_LONG *bsr)
{
  LOG_D(NR_MAC,
        "LONG BSR, LCG ID(7-0) %d/%d/%d/%d/%d/%d/%d/%d\n",
        bsr->LcgID7,
        bsr->LcgID6,
        bsr->LcgID5,
        bsr->LcgID4,
        bsr->LcgID3,
        bsr->LcgID2,
        bsr->LcgID1,
        bsr->LcgID0);
  bool bsr_active[8] = {bsr->LcgID0 != 0, bsr->LcgID1 != 0, bsr->LcgID2 != 0, bsr->LcgID3 != 0, bsr->LcgID4 != 0, bsr->LcgID5 != 0, bsr->LcgID6 != 0, bsr->LcgID7 != 0};

  int estim_size = 0;
  int max = sizeofArray(NR_LONG_BSR_TABLE) - 1;
  uint8_t *payload = ((uint8_t*) bsr) + 1;
  int m = 0;
  const int total_lcgids = 8; /* see 38.321 6.1.3.1 */
  for (int n = 0; n < total_lcgids; n++) {
    if (!bsr_active[n])
      continue;
    int rep_idx = payload[m];
    int estim_idx = overestim_bsr_index(rep_idx);
    int idx = min(estim_idx, max);
    estim_size += NR_LONG_BSR_TABLE[idx];

    LOG_D(NR_MAC, "LONG BSR LCGID/m %d/%d Index %d estim index %d size %d", n, m, rep_idx, estim_idx, estim_size);
    m++;
  }
  return estim_size;
}

//  For both UL-SCH except:
//   - UL-SCH: fixed-size MAC CE(known by LCID)
//   - UL-SCH: padding
//   - UL-SCH: MSG3 48-bits
//  |0|1|2|3|4|5|6|7|  bit-wise
//  |R|F|   LCID    |
//  |       L       |
//  |0|1|2|3|4|5|6|7|  bit-wise
//  |R|F|   LCID    |
//  |       L       |
//  |       L       |
//
//  For:
//   - UL-SCH: fixed-size MAC CE(known by LCID)
//   - UL-SCH: padding, for single/multiple 1-oct padding CE(s)
//   - UL-SCH: MSG3 48-bits
//  |0|1|2|3|4|5|6|7|  bit-wise
//  |R|R|   LCID    |
//
//  LCID: The Logical Channel ID field identifies the logical channel instance of the corresponding MAC SDU or the type of the corresponding MAC CE or padding as described in Tables 6.2.1-1 and 6.2.1-2 for the DL-SCH and UL-SCH respectively. There is one LCID field per MAC subheader. The LCID field size is 6 bits;
//  L: The Length field indicates the length of the corresponding MAC SDU or variable-sized MAC CE in bytes. There is one L field per MAC subheader except for subheaders corresponding to fixed-sized MAC CEs and padding. The size of the L field is indicated by the F field;
//  F: length of L is 0:8 or 1:16 bits wide
//  R: Reserved bit, set to zero.

// return: length of subPdu header
// 3GPP TS 38.321 Section 6
uint8_t decode_ul_mac_sub_pdu_header(uint8_t *pduP, uint8_t *lcid, uint16_t *length)
{
  uint16_t mac_subheader_len = 1;
  *lcid = pduP[0] & 0x3F;

  switch (*lcid) {
    case UL_SCH_LCID_CCCH_64_BITS:
      *length = 8;
      break;
    case UL_SCH_LCID_SRB1:
    case UL_SCH_LCID_SRB2:
    case UL_SCH_LCID_DTCH ...(UL_SCH_LCID_DTCH + 28):
    case UL_SCH_LCID_L_TRUNCATED_BSR:
    case UL_SCH_LCID_L_BSR:
      if (pduP[0] & 0x40) { // F = 1
        mac_subheader_len = 3;
        *length = (pduP[1] << 8) + pduP[2];
      } else { // F = 0
        mac_subheader_len = 2;
        *length = pduP[1];
      }
      break;
    case UL_SCH_LCID_CCCH_48_BITS:
      *length = 6;
      break;
    case UL_SCH_LCID_SINGLE_ENTRY_PHR:
    case UL_SCH_LCID_C_RNTI:
      *length = 2;
      break;
    case UL_SCH_LCID_S_TRUNCATED_BSR:
    case UL_SCH_LCID_S_BSR:
      *length = 1;
      break;
    case UL_SCH_LCID_PADDING:
      // Nothing to do
      break;
    default:
      LOG_E(NR_MAC, "LCID %0x not handled yet!\n", *lcid);
      break;
  }

  LOG_D(NR_MAC, "Decoded LCID 0x%X, header bytes: %d, payload bytes %d\n", *lcid, mac_subheader_len, *length);

  return mac_subheader_len;
}

static rnti_t lcid_crnti_lookahead(uint8_t *pdu, uint32_t pdu_len)
{
  while (pdu_len > 0) {
    uint16_t mac_len = 0;
    uint8_t lcid = 0;
    uint16_t mac_subheader_len = decode_ul_mac_sub_pdu_header(pdu, &lcid, &mac_len);
    // Check for valid PDU
    if (mac_subheader_len + mac_len > pdu_len) {
      LOG_E(NR_MAC,
            "Invalid PDU! mac_subheader_len: %d, mac_len: %d, remaining pdu_len: %d\n",
            mac_subheader_len,
            mac_len,
            pdu_len);

      LOG_E(NR_MAC, "Residual UL MAC PDU: ");
      uint32_t print_len = pdu_len > 30 ? 30 : pdu_len; // Only printf 1st - 30nd bytes
      for (int i = 0; i < print_len; i++)
        printf("%02x ", pdu[i]);
      printf("\n");
      return 0;
    }

    if (lcid == UL_SCH_LCID_C_RNTI) {
      // Extract C-RNTI value
      rnti_t crnti = ((pdu[1] & 0xFF) << 8) | (pdu[2] & 0xFF);
      LOG_D(NR_MAC, "Received a MAC CE for C-RNTI with %04x\n", crnti);
      return crnti;
    } else if (lcid == UL_SCH_LCID_PADDING) {
      // End of MAC PDU, can ignore the remaining bytes
      return 0;
    }

    pdu += mac_len + mac_subheader_len;
    pdu_len -= mac_len + mac_subheader_len;
  }
  return 0;
}

static int nr_process_mac_pdu(instance_t module_idP,
                              NR_UE_info_t *UE,
                              uint8_t CC_id,
                              frame_t frameP,
                              sub_frame_t slot,
                              uint8_t *pduP,
                              uint32_t pdu_len,
                              const int8_t harq_pid)
{
  int sdus = 0;
  NR_UE_UL_BWP_t *ul_bwp = &UE->current_UL_BWP;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;

  if (pduP[0] != UL_SCH_LCID_PADDING) {
    ws_trace_t tmp = {.nr = true,
                      .direction = DIRECTION_UPLINK,
                      .pdu_buffer = pduP,
                      .pdu_buffer_size = pdu_len,
                      .ueid = 0,
                      .rntiType = WS_C_RNTI,
                      .rnti = UE->rnti,
                      .sysFrame = frameP,
                      .subframe = slot,
                      .harq_pid = harq_pid};
    trace_pdu(&tmp);
  }

#ifdef ENABLE_MAC_PAYLOAD_DEBUG
  LOG_I(NR_MAC, "In %s: dumping MAC PDU in %d.%d:\n", __func__, frameP, slot);
  log_dump(NR_MAC, pduP, pdu_len, LOG_DUMP_CHAR, "\n");
#endif

  while (pdu_len > 0) {
    uint16_t mac_len = 0;
    uint8_t lcid = 0;

    uint16_t mac_subheader_len = decode_ul_mac_sub_pdu_header(pduP, &lcid, &mac_len);
    // Check for valid PDU
    if (mac_subheader_len + mac_len > pdu_len) {
      LOG_E(NR_MAC,
            "Invalid PDU in %d.%d for RNTI %04X! mac_subheader_len: %d, mac_len: %d, remaining pdu_len: %d\n",
            frameP,
            slot,
            UE->rnti,
            mac_subheader_len,
            mac_len,
            pdu_len);

      LOG_E(NR_MAC, "Residual UL MAC PDU: ");
      int print_len = pdu_len > 30 ? 30 : pdu_len; // Only printf 1st - 30nd bytes
      for (int i = 0; i < print_len; i++)
        printf("%02x ", pduP[i]);
      printf("\n");
      return 0;
    }

    LOG_D(NR_MAC,
          "Received UL-SCH sub-PDU with LCID 0x%X in %d.%d for RNTI %04X (remaining PDU length %d)\n",
          lcid,
          frameP,
          slot,
          UE->rnti,
          pdu_len);

    unsigned char *ce_ptr;

    switch (lcid) {
      case UL_SCH_LCID_CCCH_64_BITS:
      case UL_SCH_LCID_CCCH_48_BITS:
        if (lcid == UL_SCH_LCID_CCCH_64_BITS) {
          // Check if it is a valid CCCH1 message, we get all 00's messages very often
          bool valid_pdu = false;
          for (int i = 0; i < (mac_subheader_len + mac_len); i++) {
            if (pduP[i] != 0) {
              valid_pdu = true;
              break;
            }
          }
          if (!valid_pdu) {
            LOG_D(NR_MAC, "%s() Invalid CCCH1 message!, pdu_len: %d\n", __func__, pdu_len);
            return 0;
          }
        }

        LOG_I(MAC, "[RAPROC] Received SDU for CCCH length %d for UE %04x\n", mac_len, UE->rnti);

        if (prepare_initial_ul_rrc_message(RC.nrmac[module_idP], UE)) {
          mac_rlc_data_ind(module_idP,
                           UE->rnti,
                           module_idP,
                           frameP,
                           ENB_FLAG_YES,
                           MBMS_FLAG_NO,
                           0,
                           (char *)(pduP + mac_subheader_len),
                           mac_len,
                           1,
                           NULL);
        } else {
          LOG_E(NR_MAC, "prepare_initial_ul_rrc_message() returned false, cannot forward CCCH message\n");
        }
        break;

      case UL_SCH_LCID_SRB1:
      case UL_SCH_LCID_SRB2:
        AssertFatal(UE->CellGroup,
                    "UE %04x %d.%d: Received LCID %d which is not configured (UE has no CellGroup)\n",
                    UE->rnti,
                    frameP,
                    slot,
                    lcid);

        mac_rlc_data_ind(module_idP,
                         UE->rnti,
                         module_idP,
                         frameP,
                         ENB_FLAG_YES,
                         MBMS_FLAG_NO,
                         lcid,
                         (char *)(pduP + mac_subheader_len),
                         mac_len,
                         1,
                         NULL);

        UE->mac_stats.ul.total_sdu_bytes += mac_len;
        UE->mac_stats.ul.lc_bytes[lcid] += mac_len;
        break;

      case UL_SCH_LCID_DTCH ...(UL_SCH_LCID_DTCH + 28):
        LOG_D(NR_MAC,
              "[UE %04x] %d.%d : ULSCH -> UL-%s %d (gNB %ld, %d bytes)\n",
              UE->rnti,
              frameP,
              slot,
              lcid < 4 ? "DCCH" : "DTCH",
              lcid,
              module_idP,
              mac_len);
        UE->mac_stats.ul.lc_bytes[lcid] += mac_len;

        mac_rlc_data_ind(module_idP,
                         UE->rnti,
                         module_idP,
                         frameP,
                         ENB_FLAG_YES,
                         MBMS_FLAG_NO,
                         lcid,
                         (char *)(pduP + mac_subheader_len),
                         mac_len,
                         1,
                         NULL);

        sdus += 1;
        /* Updated estimated buffer when receiving data */
        if (sched_ctrl->estimated_ul_buffer >= mac_len)
          sched_ctrl->estimated_ul_buffer -= mac_len;
        else
          sched_ctrl->estimated_ul_buffer = 0;
        break;

      case UL_SCH_LCID_RECOMMENDED_BITRATE_QUERY:
        // 38.321 Ch6.1.3.20
        break;

      case UL_SCH_LCID_MULTI_ENTRY_PHR_4_OCT:
        LOG_E(NR_MAC, "Multi entry PHR not supported\n");
        break;

      case UL_SCH_LCID_CONFIGURED_GRANT_CONFIRMATION:
        // 38.321 Ch6.1.3.7
        break;

      case UL_SCH_LCID_MULTI_ENTRY_PHR_1_OCT:
        LOG_E(NR_MAC, "Multi entry PHR not supported\n");
        break;

      case UL_SCH_LCID_SINGLE_ENTRY_PHR:
        if (harq_pid < 0) {
          LOG_E(NR_MAC, "Invalid HARQ PID %d\n", harq_pid);
          return 0;
        }
        NR_sched_pusch_t *sched_pusch = &sched_ctrl->ul_harq_processes[harq_pid].sched_pusch;

        /* Extract SINGLE ENTRY PHR elements for PHR calculation */
        ce_ptr = &pduP[mac_subheader_len];
        NR_SINGLE_ENTRY_PHR_MAC_CE *phr = (NR_SINGLE_ENTRY_PHR_MAC_CE *)ce_ptr;
        /* Save the phr info */
        int PH;
        const int PCMAX = phr->PCMAX;
        /* 38.133 Table10.1.17.1-1 */
        if (phr->PH < 55) {
          PH = phr->PH - 32;
        } else if (phr->PH < 63) {
          PH = 24 + (phr->PH - 55) * 2;
        } else {
          PH = 38;
        }
        // in sched_ctrl we set normalized PH wrt MCS and PRBs
        long *deltaMCS = ul_bwp->pusch_Config ? ul_bwp->pusch_Config->pusch_PowerControl->deltaMCS : NULL;
        sched_ctrl->ph = PH
                         + compute_ph_factor(ul_bwp->scs,
                                             sched_pusch->tb_size << 3,
                                             sched_pusch->rbSize,
                                             sched_pusch->nrOfLayers,
                                             sched_pusch->tda_info.nrOfSymbols, // n_symbols
                                             sched_pusch->dmrs_info.num_dmrs_symb * sched_pusch->dmrs_info.N_PRB_DMRS, // n_dmrs
                                             deltaMCS,
                                             true);
        sched_ctrl->ph0 = PH;
        /* 38.133 Table10.1.18.1-1 */
        sched_ctrl->pcmax = PCMAX - 29;
        LOG_D(NR_MAC,
              "SINGLE ENTRY PHR %d.%d R1 %d PH %d (%d dB) R2 %d PCMAX %d (%d dBm)\n",
              frameP,
              slot,
              phr->R1,
              PH,
              sched_ctrl->ph,
              phr->R2,
              PCMAX,
              sched_ctrl->pcmax);
        break;

      case UL_SCH_LCID_C_RNTI:
        for (int i = 0; i < NR_NB_RA_PROC_MAX; i++) {
          NR_RA_t *ra = &RC.nrmac[module_idP]->common_channels[CC_id].ra[i];
          if (ra->ra_state == nrRA_gNB_IDLE && ra->rnti == UE->rnti) {
            // Extract C-RNTI value
            rnti_t crnti = ((pduP[1] & 0xFF) << 8) | (pduP[2] & 0xFF);
            AssertFatal(false,
                        "Received MAC CE for C-RNTI %04x without RA running, procedure exists? Or is it a bug while decoding the "
                        "MAC PDU?\n",
                        crnti);
            break;
          }
        }
        break;

      case UL_SCH_LCID_S_TRUNCATED_BSR:
      case UL_SCH_LCID_S_BSR:
        /* Extract short BSR value */
        ce_ptr = &pduP[mac_subheader_len];
        sched_ctrl->estimated_ul_buffer = estimate_ul_buffer_short_bsr((NR_BSR_SHORT *)ce_ptr);
        LOG_D(NR_MAC, "SHORT BSR at %4d.%2d, est buf %d\n", frameP, slot, sched_ctrl->estimated_ul_buffer);
        break;

      case UL_SCH_LCID_L_TRUNCATED_BSR:
      case UL_SCH_LCID_L_BSR:
        /* Extract long BSR value */
        ce_ptr = &pduP[mac_subheader_len];
        sched_ctrl->estimated_ul_buffer = estimate_ul_buffer_long_bsr((NR_BSR_LONG *)ce_ptr);
        LOG_D(NR_MAC, "LONG BSR at %4d.%2d, estim buf %d\n", frameP, slot, sched_ctrl->estimated_ul_buffer);
        break;

      case UL_SCH_LCID_PADDING:
        // End of MAC PDU, can ignore the rest.
        return 0;

      default:
        LOG_E(NR_MAC, "RNTI %0x [%d.%d], received unknown MAC header (LCID = 0x%02x)\n", UE->rnti, frameP, slot, lcid);
        return -1;
        break;
    }

#ifdef ENABLE_MAC_PAYLOAD_DEBUG
    if (lcid < 45 || lcid == 52 || lcid == 63) {
      LOG_I(NR_MAC, "In %s: dumping UL MAC SDU sub-header with length %d (LCID = 0x%02x):\n", __func__, mac_subheader_len, lcid);
      log_dump(NR_MAC, pduP, mac_subheader_len, LOG_DUMP_CHAR, "\n");
      LOG_I(NR_MAC, "In %s: dumping UL MAC SDU with length %d (LCID = 0x%02x):\n", __func__, mac_len, lcid);
      log_dump(NR_MAC, pduP + mac_subheader_len, mac_len, LOG_DUMP_CHAR, "\n");
    } else {
      LOG_I(NR_MAC, "In %s: dumping UL MAC CE with length %d (LCID = 0x%02x):\n", __func__, mac_len, lcid);
      log_dump(NR_MAC, pduP + mac_subheader_len + mac_len, mac_len, LOG_DUMP_CHAR, "\n");
    }
#endif

    pduP += (mac_subheader_len + mac_len);
    pdu_len -= (mac_subheader_len + mac_len);
  }

  UE->mac_stats.ul.num_mac_sdu += sdus;

  return 0;
}

static void finish_nr_ul_harq(NR_UE_sched_ctrl_t *sched_ctrl, int harq_pid)
{
  NR_UE_ul_harq_t *harq = &sched_ctrl->ul_harq_processes[harq_pid];

  harq->ndi ^= 1;
  harq->round = 0;

  add_tail_nr_list(&sched_ctrl->available_ul_harq, harq_pid);
}

static void abort_nr_ul_harq(NR_UE_info_t *UE, int8_t harq_pid)
{
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_UE_ul_harq_t *harq = &sched_ctrl->ul_harq_processes[harq_pid];

  finish_nr_ul_harq(sched_ctrl, harq_pid);
  UE->mac_stats.ul.errors++;

  /* the transmission failed: the UE won't send the data we expected initially,
   * so retrieve to correctly schedule after next BSR */
  sched_ctrl->sched_ul_bytes -= harq->sched_pusch.tb_size;
  if (sched_ctrl->sched_ul_bytes < 0)
    sched_ctrl->sched_ul_bytes = 0;
}

static bool get_UE_waiting_CFRA_msg3(const gNB_MAC_INST *gNB_mac,
                                     const int CC_id,
                                     const frame_t frame,
                                     const sub_frame_t slot,
                                     rnti_t rnti)
{
  bool UE_waiting_CFRA_msg3 = false;
  for (int i = 0; i < NR_NB_RA_PROC_MAX; i++) {
    const NR_RA_t *ra = &gNB_mac->common_channels[CC_id].ra[i];
    if (ra->cfra && ra->ra_state == nrRA_WAIT_Msg3 && frame == ra->Msg3_frame && slot == ra->Msg3_slot && rnti == ra->rnti) {
      UE_waiting_CFRA_msg3 = true;
      break;
    }
  }
  return UE_waiting_CFRA_msg3;
}

void handle_nr_ul_harq(const int CC_idP,
                       module_id_t mod_id,
                       frame_t frame,
                       sub_frame_t slot,
                       const nfapi_nr_crc_t *crc_pdu)
{
  gNB_MAC_INST *nrmac = RC.nrmac[mod_id];
  if (nrmac->radio_config.disable_harq) {
    LOG_D(NR_MAC, "skipping UL feedback handling as HARQ is disabled\n");
    return;
  }

  NR_SCHED_LOCK(&nrmac->sched_lock);
  for (int i = 0; i < NR_NB_RA_PROC_MAX; ++i) {
    NR_RA_t *ra = &nrmac->common_channels[CC_idP].ra[i];
    // if we find any ongoing RA that has already scheduled MSG3
    // and it is expecting its reception in current frame and slot with matching RNTI
    // we can exit the function (no HARQ involved)
    if (ra->ra_state >= nrRA_WAIT_Msg3 && ra->rnti == crc_pdu->rnti && frame == ra->Msg3_frame && slot == ra->Msg3_slot) {
      LOG_D(NR_MAC, "UL for rnti %04x in RA (MSG3), no need to process HARQ\n", crc_pdu->rnti);
      NR_SCHED_UNLOCK(&nrmac->sched_lock);
      return;
    }
  }

  NR_UE_info_t *UE = find_nr_UE(&nrmac->UE_info, crc_pdu->rnti);
  if (!UE) {
    NR_SCHED_UNLOCK(&nrmac->sched_lock);
    LOG_E(NR_MAC, "Couldn't identify UE connected with current UL HARQ process\n");
    return;
  }

  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  int8_t harq_pid = sched_ctrl->feedback_ul_harq.head;
  LOG_D(NR_MAC, "Comparing crc_pdu->harq_id vs feedback harq_pid = %d %d\n",crc_pdu->harq_id, harq_pid);
  while (crc_pdu->harq_id != harq_pid || harq_pid < 0) {
    LOG_W(NR_MAC, "Unexpected ULSCH HARQ PID %d (have %d) for RNTI 0x%04x\n", crc_pdu->harq_id, harq_pid, crc_pdu->rnti);
    if (harq_pid < 0) {
      NR_SCHED_UNLOCK(&nrmac->sched_lock);
      return;
    }

    remove_front_nr_list(&sched_ctrl->feedback_ul_harq);
    sched_ctrl->ul_harq_processes[harq_pid].is_waiting = false;

    if(sched_ctrl->ul_harq_processes[harq_pid].round >= RC.nrmac[mod_id]->ul_bler.harq_round_max - 1) {
      abort_nr_ul_harq(UE, harq_pid);
    } else {
      sched_ctrl->ul_harq_processes[harq_pid].round++;
      add_tail_nr_list(&sched_ctrl->retrans_ul_harq, harq_pid);
    }
    harq_pid = sched_ctrl->feedback_ul_harq.head;
  }
  remove_front_nr_list(&sched_ctrl->feedback_ul_harq);
  NR_UE_ul_harq_t *harq = &sched_ctrl->ul_harq_processes[harq_pid];
  DevAssert(harq->is_waiting);
  harq->feedback_slot = -1;
  harq->is_waiting = false;
  if (!crc_pdu->tb_crc_status) {
    finish_nr_ul_harq(sched_ctrl, harq_pid);
    LOG_D(NR_MAC,
          "Ulharq id %d crc passed for RNTI %04x\n",
          harq_pid,
          crc_pdu->rnti);
  } else if (harq->round >= RC.nrmac[mod_id]->ul_bler.harq_round_max  - 1) {
    abort_nr_ul_harq(UE, harq_pid);
    LOG_D(NR_MAC,
          "RNTI %04x: Ulharq id %d crc failed in all rounds\n",
          crc_pdu->rnti,
          harq_pid);
  } else {
    harq->round++;
    LOG_D(NR_MAC,
          "Ulharq id %d crc failed for RNTI %04x\n",
          harq_pid,
          crc_pdu->rnti);
    add_tail_nr_list(&sched_ctrl->retrans_ul_harq, harq_pid);
  }
  NR_SCHED_UNLOCK(&nrmac->sched_lock);
}

static void handle_msg3_failed_rx(NR_RA_t *ra, int i, int harq_round_max)
{
  // for CFRA (NSA) do not schedule retransmission of msg3
  if (ra->cfra) {
    LOG_D(NR_MAC, "Random Access %i failed at state %s (NSA msg3 reception failed)\n", i, nrra_text[ra->ra_state]);
    nr_clear_ra_proc(ra);
    return;
  }

  if (ra->msg3_round >= harq_round_max - 1) {
    LOG_W(NR_MAC, "Random Access %i failed at state %s (Reached msg3 max harq rounds)\n", i, nrra_text[ra->ra_state]);
    nr_clear_ra_proc(ra);
    return;
  }

  LOG_D(NR_MAC, "Random Access %i Msg3 CRC did not pass\n", i);
  ra->msg3_round++;
  ra->ra_state = nrRA_Msg3_retransmission;
}

/*
* When data are received on PHY and transmitted to MAC
*/
static void _nr_rx_sdu(const module_id_t gnb_mod_idP,
                       const int CC_idP,
                       const frame_t frameP,
                       const sub_frame_t slotP,
                       const rnti_t rntiP,
                       uint8_t *sduP,
                       const uint32_t sdu_lenP,
                       const int8_t harq_pid,
                       const uint16_t timing_advance,
                       const uint8_t ul_cqi,
                       const uint16_t rssi)
{
  gNB_MAC_INST *gNB_mac = RC.nrmac[gnb_mod_idP];

  const int current_rnti = rntiP;
  LOG_D(NR_MAC, "rx_sdu for rnti %04x\n", current_rnti);
  const int target_snrx10 = gNB_mac->pusch_target_snrx10;
  const int rssi_threshold = gNB_mac->pusch_rssi_threshold;
  const int pusch_failure_thres = gNB_mac->pusch_failure_thres;

  NR_UE_info_t *UE = find_nr_UE(&gNB_mac->UE_info, current_rnti);
  bool UE_waiting_CFRA_msg3 = get_UE_waiting_CFRA_msg3(gNB_mac, CC_idP, frameP, slotP, current_rnti);

  if (UE && UE_waiting_CFRA_msg3 == false) {

    NR_UE_sched_ctrl_t *UE_scheduling_control = &UE->UE_sched_ctrl;

    if (sduP)
      T(T_GNB_MAC_UL_PDU_WITH_DATA, T_INT(gnb_mod_idP), T_INT(CC_idP),
        T_INT(rntiP), T_INT(frameP), T_INT(slotP), T_INT(harq_pid),
        T_BUFFER(sduP, sdu_lenP));

    UE->mac_stats.ul.total_bytes += sdu_lenP;
    LOG_D(NR_MAC, "[gNB %d][PUSCH %d] CC_id %d %d.%d Received ULSCH sdu from PHY (rnti %04x) ul_cqi %d TA %d sduP %p, rssi %d\n",
          gnb_mod_idP,
          harq_pid,
          CC_idP,
          frameP,
          slotP,
          current_rnti,
          ul_cqi,
          timing_advance,
          sduP,
          rssi);
    if (harq_pid < 0) {
      LOG_E(NR_MAC, "UE %04x received ULSCH when feedback UL HARQ %d (unexpected ULSCH transmission)\n", rntiP, harq_pid);
      return;
    }

    // if not missed detection (10dB threshold for now)
    if (rssi > 0) {
      int txpower_calc = UE_scheduling_control->ul_harq_processes[harq_pid].sched_pusch.phr_txpower_calc;
      UE->mac_stats.deltaMCS = txpower_calc;
      UE->mac_stats.NPRB = UE_scheduling_control->ul_harq_processes[harq_pid].sched_pusch.rbSize;
      if (ul_cqi != 0xff)
        UE_scheduling_control->tpc0 = nr_get_tpc(target_snrx10, ul_cqi, 30, txpower_calc);
      if (UE_scheduling_control->ph < 0 && UE_scheduling_control->tpc0 > 1)
        UE_scheduling_control->tpc0 = 1;

      UE_scheduling_control->tpc0 = nr_limit_tpc(UE_scheduling_control->tpc0, rssi, rssi_threshold);

      if (timing_advance != 0xffff)
        UE_scheduling_control->ta_update = timing_advance;
      UE_scheduling_control->raw_rssi = rssi;
      UE_scheduling_control->pusch_snrx10 = ul_cqi * 5 - 640 - (txpower_calc * 10);

      if (UE_scheduling_control->tpc0 > 1)
        LOG_D(NR_MAC,
              "[UE %04x] %d.%d. PUSCH TPC %d and TA %d pusch_snrx10 %d rssi %d phrx_tx_power %d PHR (1PRB) %d mcs %d, nb_rb %d\n",
              UE->rnti,
              frameP,
              slotP,
              UE_scheduling_control->tpc0,
              UE_scheduling_control->ta_update,
              UE_scheduling_control->pusch_snrx10,
              UE_scheduling_control->raw_rssi,
              txpower_calc,
              UE_scheduling_control->ph,
              UE_scheduling_control->ul_harq_processes[harq_pid].sched_pusch.mcs,
              UE_scheduling_control->ul_harq_processes[harq_pid].sched_pusch.rbSize);

      NR_UE_ul_harq_t *cur_harq = &UE_scheduling_control->ul_harq_processes[harq_pid];
      if (cur_harq->round == 0)
       UE->mac_stats.pusch_snrx10 = UE_scheduling_control->pusch_snrx10;
      LOG_D(NR_MAC, "[UE %04x] PUSCH TPC %d and TA %d\n",UE->rnti,UE_scheduling_control->tpc0,UE_scheduling_control->ta_update);
    }
    else{
      LOG_D(NR_MAC,"[UE %04x] Detected DTX : increasing UE TX power\n",UE->rnti);
      UE_scheduling_control->tpc0 = 1;
    }

#if defined(ENABLE_MAC_PAYLOAD_DEBUG)

    LOG_I(NR_MAC, "Printing received UL MAC payload at gNB side: %d \n");
    for (uint32_t i = 0; i < sdu_lenP; i++) {
      // harq_process_ul_ue->a[i] = (unsigned char) rand();
      // printf("a[%d]=0x%02x\n",i,harq_process_ul_ue->a[i]);
      printf("%02x ", (unsigned char)sduP[i]);
    }
    printf("\n");

#endif

    if (sduP != NULL) {
      LOG_D(NR_MAC, "Received PDU at MAC gNB \n");

      UE->UE_sched_ctrl.pusch_consecutive_dtx_cnt = 0;
      UE_scheduling_control->sched_ul_bytes -= sdu_lenP;
      if (UE_scheduling_control->sched_ul_bytes < 0)
        UE_scheduling_control->sched_ul_bytes = 0;

      nr_process_mac_pdu(gnb_mod_idP, UE, CC_idP, frameP, slotP, sduP, sdu_lenP, harq_pid);
    }
    else {
      if (ul_cqi == 0xff || ul_cqi <= 128) {
        UE->UE_sched_ctrl.pusch_consecutive_dtx_cnt++;
        UE->mac_stats.ulsch_DTX++;
      }

      if (!get_softmodem_params()->phy_test && UE->UE_sched_ctrl.pusch_consecutive_dtx_cnt >= pusch_failure_thres) {
        LOG_W(NR_MAC,
              "UE %04x: Detected UL Failure on PUSCH after %d PUSCH DTX, stopping scheduling\n",
              UE->rnti,
              UE->UE_sched_ctrl.pusch_consecutive_dtx_cnt);
        nr_mac_trigger_ul_failure(&UE->UE_sched_ctrl, UE->current_UL_BWP.scs);
      }
    }
  } else if (sduP) {

    bool no_sig = true;
    for (uint32_t k = 0; k < sdu_lenP; k++) {
      if (sduP[k] != 0) {
        no_sig = false;
        break;
      }
    }

    T(T_GNB_MAC_UL_PDU_WITH_DATA, T_INT(gnb_mod_idP), T_INT(CC_idP),
      T_INT(rntiP), T_INT(frameP), T_INT(slotP), T_INT(-1) /* harq_pid */,
      T_BUFFER(sduP, sdu_lenP));
    
    /* we don't know this UE (yet). Check whether there is a ongoing RA (Msg 3)
     * and check the corresponding UE's RNTI match, in which case we activate
     * it. */
    for (int i = 0; i < NR_NB_RA_PROC_MAX; ++i) {
      NR_RA_t *ra = &gNB_mac->common_channels[CC_idP].ra[i];
      if (ra->ra_type == RA_4_STEP && ra->ra_state != nrRA_WAIT_Msg3)
        continue;

      if (no_sig) {
        LOG_W(NR_MAC, "Random Access %i ULSCH with no signal\n", i);
        handle_msg3_failed_rx(ra, i, gNB_mac->ul_bler.harq_round_max);
        continue;
      }
      if (ra->ra_type == RA_2_STEP) {
        // random access pusch with RA-RNTI
        if (ra->RA_rnti != current_rnti) {
          LOG_E(NR_MAC, "expected TC_RNTI %04x to match current RNTI %04x\n", ra->RA_rnti, current_rnti);
          continue;
        }
      } else {
        // random access pusch with TC-RNTI
        if (ra->rnti != current_rnti) {
          LOG_E(NR_MAC, "expected TC_RNTI %04x to match current RNTI %04x\n", ra->rnti, current_rnti);

          if ((frameP == ra->Msg3_frame) && (slotP == ra->Msg3_slot)) {
            LOG_W(NR_MAC,
                  "Random Access %i failed at state %s (TC_RNTI %04x RNTI %04x)\n",
                  i,
                  nrra_text[ra->ra_state],
                  ra->rnti,
                  current_rnti);
            nr_clear_ra_proc(ra);
          }

          continue;
        }
      }

      UE = UE ? UE : add_new_nr_ue(gNB_mac, ra->rnti, ra->CellGroup);
      if (!UE) {
        LOG_W(NR_MAC,
              "Random Access %i discarded at state %s (TC_RNTI %04x RNTI %04x): max number of users achieved!\n",
              i,
              nrra_text[ra->ra_state],
              ra->rnti,
              current_rnti);

        nr_clear_ra_proc(ra);
        return;
      }

      UE->UE_beam_index = ra->beam_id;

      // re-initialize ta update variables after RA procedure completion
      UE->UE_sched_ctrl.ta_frame = frameP;

      LOG_A(NR_MAC, "%4d.%2d PUSCH with TC_RNTI 0x%04x received correctly\n", frameP, slotP, current_rnti);

      NR_UE_sched_ctrl_t *UE_scheduling_control = &UE->UE_sched_ctrl;
      if (ul_cqi != 0xff) {
        UE_scheduling_control->tpc0 = nr_get_tpc(target_snrx10, ul_cqi, 30, UE_scheduling_control->sched_pusch.phr_txpower_calc);
        UE_scheduling_control->pusch_snrx10 = ul_cqi * 5 - 640 - UE_scheduling_control->sched_pusch.phr_txpower_calc * 10;
      }
      if (timing_advance != 0xffff)
        UE_scheduling_control->ta_update = timing_advance;
      UE_scheduling_control->raw_rssi = rssi;
      LOG_D(NR_MAC, "[UE %04x] PUSCH TPC %d and TA %d\n", UE->rnti, UE_scheduling_control->tpc0, UE_scheduling_control->ta_update);
      if (ra->cfra) {
        LOG_A(NR_MAC, "(rnti 0x%04x) CFRA procedure succeeded!\n", ra->rnti);
        nr_mac_reset_ul_failure(UE_scheduling_control);
        reset_dl_harq_list(UE_scheduling_control);
        reset_ul_harq_list(UE_scheduling_control);
        process_addmod_bearers_cellGroupConfig(&UE->UE_sched_ctrl, ra->CellGroup->rlc_BearerToAddModList);
        nr_clear_ra_proc(ra);
      } else {
        LOG_D(NR_MAC, "[RAPROC] Received %s:\n", ra->ra_type == RA_2_STEP ? "MsgA-PUSCH" : "Msg3");
        for (uint32_t k = 0; k < sdu_lenP; k++) {
          LOG_D(NR_MAC, "(%i): 0x%x\n", k, sduP[k]);
        }

        // 3GPP TS 38.321 Section 5.4.3 Multiplexing and assembly
        // Logical channels shall be prioritised in accordance with the following order (highest priority listed first):
        // - MAC CE for C-RNTI, or data from UL-CCCH;
        // This way, we need to process MAC CE for C-RNTI if RA is active and it is present in the MAC PDU
        // Search for MAC CE for C-RNTI
        rnti_t crnti = lcid_crnti_lookahead(sduP, sdu_lenP);
        if (crnti != 0) { // 3GPP TS 38.321 Table 7.1-1: RNTI values, RNTI 0x0000: N/A
          // this UE is the one identified by the RNTI in sduP
          ra->rnti = crnti;
          // Remove UE context just created after Msg.3 in some milliseconds as the UE is one already known (not now, as the UE
          // context is still needed for the moment)
          nr_mac_trigger_release_timer(&UE->UE_sched_ctrl, UE->current_UL_BWP.scs);

          // Replace the current UE by the UE identified by C-RNTI
          UE = find_nr_UE(&gNB_mac->UE_info, crnti);
          if (!UE) {
            // The UE identified by C-RNTI no longer exists at the gNB
            // Let's abort the current RA, so the UE will trigger a new RA later but using RRCSetupRequest instead. A better
            // solution may be implemented
            LOG_W(NR_MAC, "No UE found with C-RNTI %04x, ignoring Msg3 to have UE come back with new RA attempt\n", ra->rnti);
            mac_remove_nr_ue(gNB_mac, ra->rnti);
            nr_clear_ra_proc(ra);
            return;
          }

          // The UE identified by C-RNTI still exists at the gNB
          // Reset Msg4_ACKed to not schedule ULSCH and DLSCH before RRC Reconfiguration
          UE->Msg4_MsgB_ACKed = false;
          nr_mac_reset_ul_failure(&UE->UE_sched_ctrl);
          // Reset HARQ processes
          reset_dl_harq_list(&UE->UE_sched_ctrl);
          reset_ul_harq_list(&UE->UE_sched_ctrl);

          // Switch to BWP where RA is configured, typically in the InitialBWP
          // At this point, UE already switched and triggered RA in that BWP, need to do BWP switching also at gNB for C-RNTI
          if (ra->DL_BWP.bwp_id != UE->current_DL_BWP.bwp_id || ra->UL_BWP.bwp_id != UE->current_UL_BWP.bwp_id) {
            LOG_D(NR_MAC, "UE %04x Switch BWP from %ld to BWP id %ld\n", UE->rnti, UE->current_DL_BWP.bwp_id, ra->DL_BWP.bwp_id);
            NR_ServingCellConfigCommon_t *scc = gNB_mac->common_channels[CC_idP].ServingCellConfigCommon;
            configure_UE_BWP(gNB_mac, scc, &UE->UE_sched_ctrl, NULL, UE, ra->DL_BWP.bwp_id, ra->UL_BWP.bwp_id);
          }

          if (UE->reconfigCellGroup) {
            // Nothing to do
            // A RRCReconfiguration message should be already pending (for example, an ongoing RRCReestablishment), and it will be
            // transmitted in Msg4
          } else {
            // Trigger RRC Reconfiguration
            LOG_I(NR_MAC, "Received UL_SCH_LCID_C_RNTI with C-RNTI 0x%04x, triggering RRC Reconfiguration\n", UE->rnti);
            nr_mac_trigger_reconfiguration(gNB_mac, UE);
          }
        } else {
          // UE Contention Resolution Identity
          // Store the first 48 bits belonging to the uplink CCCH SDU within Msg3 to fill in Msg4
          // First byte corresponds to R/LCID MAC sub-header
          memcpy(ra->cont_res_id, &sduP[1], sizeof(uint8_t) * 6);
        }

        // Decode MAC PDU for the correct UE, after checking for MAC CE for C-RNTI
        // harq_pid set a non valid value because it is not used in this call
        // the function is only called to decode the contention resolution sub-header
        nr_process_mac_pdu(gnb_mod_idP, UE, CC_idP, frameP, slotP, sduP, sdu_lenP, -1);

        LOG_I(NR_MAC,
              "Activating scheduling %s for TC_RNTI 0x%04x (state %s)\n",
              ra->ra_type == RA_2_STEP ? "MsgB" : "Msg4",
              ra->rnti,
              nrra_text[ra->ra_state]);
        ra->ra_state = ra->ra_type == RA_2_STEP ? nrRA_MsgB : nrRA_Msg4;
        LOG_D(NR_MAC, "TC_RNTI 0x%04x next RA state %s\n", ra->rnti, nrra_text[ra->ra_state]);
        return;
      }
    }
  } else {
    for (int i = 0; i < NR_NB_RA_PROC_MAX; ++i) {
      NR_RA_t *ra = &gNB_mac->common_channels[CC_idP].ra[i];
      if (ra->ra_state != nrRA_WAIT_Msg3)
        continue;

      if((frameP!=ra->Msg3_frame) || (slotP!=ra->Msg3_slot))
        continue;

      if (ul_cqi != 0xff)
        ra->msg3_TPC = nr_get_tpc(target_snrx10, ul_cqi, 30, 0);

      handle_msg3_failed_rx(ra, i, gNB_mac->ul_bler.harq_round_max);
    }
  }
}

void nr_rx_sdu(const module_id_t gnb_mod_idP,
               const int CC_idP,
               const frame_t frameP,
               const sub_frame_t slotP,
               const rnti_t rntiP,
               uint8_t *sduP,
               const uint32_t sdu_lenP,
               const int8_t harq_pid,
               const uint16_t timing_advance,
               const uint8_t ul_cqi,
               const uint16_t rssi)
{
  gNB_MAC_INST *gNB_mac = RC.nrmac[gnb_mod_idP];
  NR_SCHED_LOCK(&gNB_mac->sched_lock);
  _nr_rx_sdu(gnb_mod_idP, CC_idP, frameP, slotP, rntiP, sduP, sdu_lenP, harq_pid, timing_advance, ul_cqi, rssi);
  NR_SCHED_UNLOCK(&gNB_mac->sched_lock);
}

static uint32_t calc_power_complex(const int16_t *x, const int16_t *y, const uint32_t size)
{
  // Real part value
  int64_t sum_x = 0;
  int64_t sum_x2 = 0;
  for(int k = 0; k<size; k++) {
    sum_x = sum_x + x[k];
    sum_x2 = sum_x2 + x[k]*x[k];
  }
  uint32_t power_re = sum_x2/size - (sum_x/size)*(sum_x/size);

  // Imaginary part power
  int64_t sum_y = 0;
  int64_t sum_y2 = 0;
  for(int k = 0; k<size; k++) {
    sum_y = sum_y + y[k];
    sum_y2 = sum_y2 + y[k]*y[k];
  }
  uint32_t power_im = sum_y2/size - (sum_y/size)*(sum_y/size);

  return power_re+power_im;
}

static c16_t nr_h_times_w(c16_t h, char w)
{
  c16_t output;
    switch (w) {
      case '0': // 0
        output.r = 0;
        output.i = 0;
        break;
      case '1': // 1
        output.r = h.r;
        output.i = h.i;
        break;
      case 'n': // -1
        output.r = -h.r;
        output.i = -h.i;
        break;
      case 'j': // j
        output.r = -h.i;
        output.i = h.r;
        break;
      case 'o': // -j
        output.r = h.i;
        output.i = -h.r;
        break;
      default:
        AssertFatal(1==0,"Invalid precoder value %c\n", w);
    }
  return output;
}

static uint8_t get_max_tpmi(const NR_PUSCH_Config_t *pusch_Config,
                            const uint16_t num_ue_srs_ports,
                            const uint8_t *nrOfLayers,
                            int *additional_max_tpmi)
{
  uint8_t max_tpmi = 0;

  if (!pusch_Config
      || (pusch_Config->txConfig != NULL && *pusch_Config->txConfig == NR_PUSCH_Config__txConfig_nonCodebook)
      || num_ue_srs_ports == 1)
    return max_tpmi;

  long max_rank = *pusch_Config->maxRank;
  long *ul_FullPowerTransmission = pusch_Config->ext1 ? pusch_Config->ext1->ul_FullPowerTransmission_r16 : NULL;
  long *codebookSubset = pusch_Config->codebookSubset;

  if (num_ue_srs_ports == 2) {

    if (max_rank == 1) {
      if (ul_FullPowerTransmission && *ul_FullPowerTransmission == NR_PUSCH_Config__ext1__ul_FullPowerTransmission_r16_fullpowerMode1) {
        max_tpmi = 2;
      } else {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          max_tpmi = 1;
        } else {
          max_tpmi = 5;
        }
      }
    } else {
      if (ul_FullPowerTransmission && *ul_FullPowerTransmission == NR_PUSCH_Config__ext1__ul_FullPowerTransmission_r16_fullpowerMode1) {
        max_tpmi = *nrOfLayers == 1 ? 2 : 0;
      } else {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          max_tpmi = *nrOfLayers == 1 ? 1 : 0;
        } else {
          max_tpmi = *nrOfLayers == 1 ? 5 : 2;
        }
      }
    }

  } else if (num_ue_srs_ports == 4) {

    if (max_rank == 1) {
      if (ul_FullPowerTransmission && *ul_FullPowerTransmission == NR_PUSCH_Config__ext1__ul_FullPowerTransmission_r16_fullpowerMode1) {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          max_tpmi = 3;
          *additional_max_tpmi = 13;
        } else {
          max_tpmi = 15;
        }
      } else {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          max_tpmi = 3;
        } else if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_partialAndNonCoherent) {
          max_tpmi = 11;
        } else {
          max_tpmi = 27;
        }
      }
    } else {
      if (ul_FullPowerTransmission && *ul_FullPowerTransmission == NR_PUSCH_Config__ext1__ul_FullPowerTransmission_r16_fullpowerMode1) {
        if (max_rank == 2) {
          if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
            max_tpmi = *nrOfLayers == 1 ? 3 : 6;
            if (*nrOfLayers == 1) {
              *additional_max_tpmi = 13;
            }
          } else {
            max_tpmi = *nrOfLayers == 1 ? 15 : 13;
          }
        } else {
          if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
            switch (*nrOfLayers) {
              case 1:
                max_tpmi = 3;
                *additional_max_tpmi = 13;
                break;
              case 2:
                max_tpmi = 6;
                break;
              case 3:
                max_tpmi = 1;
                break;
              case 4:
                max_tpmi = 0;
                break;
              default:
                LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
            }
          } else {
            switch (*nrOfLayers) {
              case 1:
                max_tpmi = 15;
                break;
              case 2:
                max_tpmi = 13;
                break;
              case 3:
              case 4:
                max_tpmi = 2;
                break;
              default:
                LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
            }
          }
        }
      } else {
        if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_nonCoherent) {
          switch (*nrOfLayers) {
            case 1:
              max_tpmi = 3;
              break;
            case 2:
              max_tpmi = 5;
              break;
            case 3:
            case 4:
              max_tpmi = 0;
              break;
            default:
              LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
          }
        } else if (codebookSubset && *codebookSubset == NR_PUSCH_Config__codebookSubset_partialAndNonCoherent) {
          switch (*nrOfLayers) {
            case 1:
              max_tpmi = 11;
              break;
            case 2:
              max_tpmi = 13;
              break;
            case 3:
            case 4:
              max_tpmi = 2;
              break;
            default:
              LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
          }
        } else {
          switch (*nrOfLayers) {
            case 1:
              max_tpmi = 28;
              break;
            case 2:
              max_tpmi = 22;
              break;
            case 3:
              max_tpmi = 7;
              break;
            case 4:
              max_tpmi = 5;
              break;
            default:
              LOG_E(NR_MAC,"Number of layers %d is invalid!\n", *nrOfLayers);
          }
        }
      }
    }

  }

  return max_tpmi;
}

static void get_precoder_matrix_coef(char *w,
                                     const uint8_t ul_ri,
                                     const uint16_t num_ue_srs_ports,
                                     const long transform_precoding,
                                     const uint8_t tpmi,
                                     const uint8_t uI,
                                     int layer_idx)
{
  if (ul_ri == 0) {
    if (num_ue_srs_ports == 2) {
      *w = table_38211_6_3_1_5_1[tpmi][uI][layer_idx];
    } else {
      if (transform_precoding == NR_PUSCH_Config__transformPrecoder_enabled) {
        *w = table_38211_6_3_1_5_2[tpmi][uI][layer_idx];
      } else {
        *w = table_38211_6_3_1_5_3[tpmi][uI][layer_idx];
      }
    }
  } else if (ul_ri == 1) {
    if (num_ue_srs_ports == 2) {
      *w = table_38211_6_3_1_5_4[tpmi][uI][layer_idx];
    } else {
      *w = table_38211_6_3_1_5_5[tpmi][uI][layer_idx];
    }
  } else {
    AssertFatal(1 == 0, "Function get_precoder_matrix_coef() does not support %i layers yet!\n", ul_ri + 1);
  }
}

static int nr_srs_tpmi_estimation(const NR_PUSCH_Config_t *pusch_Config,
                                  const long transform_precoding,
                                  const uint8_t *channel_matrix,
                                  const uint8_t normalized_iq_representation,
                                  const uint16_t num_gnb_antenna_elements,
                                  const uint16_t num_ue_srs_ports,
                                  const uint16_t prg_size,
                                  const uint16_t num_prgs,
                                  const uint8_t ul_ri)
{
  if (ul_ri > 1) {
    LOG_D(NR_MAC, "TPMI computation for ul_ri %i is not implemented yet!\n", ul_ri);
    return 0;
  }

  uint8_t tpmi_sel = 0;
  const uint8_t nrOfLayers = ul_ri + 1;
  int16_t precoded_channel_matrix_re[num_prgs * num_gnb_antenna_elements];
  int16_t precoded_channel_matrix_im[num_prgs * num_gnb_antenna_elements];
  c16_t *channel_matrix16 = (c16_t *)channel_matrix;
  uint32_t max_precoded_signal_power = 0;
  int additional_max_tpmi = -1;
  char w;

  uint8_t max_tpmi = get_max_tpmi(pusch_Config, num_ue_srs_ports, &nrOfLayers, &additional_max_tpmi);
  uint8_t end_tpmi_loop = additional_max_tpmi > max_tpmi ? additional_max_tpmi : max_tpmi;

  //                      channel_matrix                          x   precoder_matrix
  // [ (gI=0,uI=0) (gI=0,uI=1) ... (gI=0,uI=num_ue_srs_ports-1) ] x   [uI=0]
  // [ (gI=1,uI=0) (gI=1,uI=1) ... (gI=1,uI=num_ue_srs_ports-1) ]     [uI=1]
  // [ (gI=2,uI=0) (gI=2,uI=1) ... (gI=2,uI=num_ue_srs_ports-1) ]     [uI=2]
  //                           ...                                     ...

  for (uint8_t tpmi = 0; tpmi <= end_tpmi_loop && end_tpmi_loop > 0; tpmi++) {
    if (tpmi > max_tpmi) {
      tpmi = end_tpmi_loop;
    }

    for (int pI = 0; pI < num_prgs; pI++) {
      for (int gI = 0; gI < num_gnb_antenna_elements; gI++) {
        uint16_t index_gI_pI = gI * num_prgs + pI;
        precoded_channel_matrix_re[index_gI_pI] = 0;
        precoded_channel_matrix_im[index_gI_pI] = 0;

        for (int uI = 0; uI < num_ue_srs_ports; uI++) {
          for (int layer_idx = 0; layer_idx < nrOfLayers; layer_idx++) {
            uint16_t index = uI * num_gnb_antenna_elements * num_prgs + index_gI_pI;
            get_precoder_matrix_coef(&w, ul_ri, num_ue_srs_ports, transform_precoding, tpmi, uI, layer_idx);
            c16_t h_times_w = nr_h_times_w(channel_matrix16[index], w);

            precoded_channel_matrix_re[index_gI_pI] += h_times_w.r;
            precoded_channel_matrix_im[index_gI_pI] += h_times_w.i;

#ifdef SRS_IND_DEBUG
            LOG_I(NR_MAC, "(pI %i, gI %i,  uI %i, layer_idx %i) w = %c, channel_matrix --> real %i, imag %i\n",
                  pI, gI, uI, layer_idx, w, channel_matrix16[index].r, channel_matrix16[index].i);
#endif
          }
        }

#ifdef SRS_IND_DEBUG
        LOG_I(NR_MAC, "(pI %i, gI %i) precoded_channel_coef --> real %i, imag %i\n",
              pI, gI, precoded_channel_matrix_re[index_gI_pI], precoded_channel_matrix_im[index_gI_pI]);
#endif
      }
    }

    uint32_t precoded_signal_power = calc_power_complex(precoded_channel_matrix_re,
                                                        precoded_channel_matrix_im,
                                                        num_prgs * num_gnb_antenna_elements);

#ifdef SRS_IND_DEBUG
    LOG_I(NR_MAC, "(tpmi %i) precoded_signal_power = %i\n", tpmi, precoded_signal_power);
#endif

    if (precoded_signal_power > max_precoded_signal_power) {
      max_precoded_signal_power = precoded_signal_power;
      tpmi_sel = tpmi;
    }
  }

  return tpmi_sel;
}

void handle_nr_srs_measurements(const module_id_t module_id,
                                const frame_t frame,
                                const sub_frame_t slot,
                                nfapi_nr_srs_indication_pdu_t *srs_ind)
{
  gNB_MAC_INST *nrmac = RC.nrmac[module_id];
  NR_SCHED_LOCK(&nrmac->sched_lock);
  LOG_D(NR_MAC, "(%d.%d) Received SRS indication for UE %04x\n", frame, slot, srs_ind->rnti);

#ifdef SRS_IND_DEBUG
  LOG_I(NR_MAC, "frame = %i\n", frame);
  LOG_I(NR_MAC, "slot = %i\n", slot);
  LOG_I(NR_MAC, "srs_ind->rnti = %04x\n", srs_ind->rnti);
  LOG_I(NR_MAC, "srs_ind->timing_advance_offset = %i\n", srs_ind->timing_advance_offset);
  LOG_I(NR_MAC, "srs_ind->timing_advance_offset_nsec = %i\n", srs_ind->timing_advance_offset_nsec);
  LOG_I(NR_MAC, "srs_ind->srs_usage = %i\n", srs_ind->srs_usage);
  LOG_I(NR_MAC, "srs_ind->report_type = %i\n", srs_ind->report_type);
#endif

  NR_UE_info_t *UE = find_nr_UE(&RC.nrmac[module_id]->UE_info, srs_ind->rnti);
  if (!UE) {
    LOG_W(NR_MAC, "Could not find UE for RNTI %04x\n", srs_ind->rnti);
    NR_SCHED_UNLOCK(&nrmac->sched_lock);
    return;
  }

  if (srs_ind->timing_advance_offset == 0xFFFF) {
    LOG_W(NR_MAC, "Invalid timing advance offset for RNTI %04x\n", srs_ind->rnti);
    NR_SCHED_UNLOCK(&nrmac->sched_lock);
    return;
  }

  gNB_MAC_INST *nr_mac = RC.nrmac[module_id];
  NR_mac_stats_t *stats = &UE->mac_stats;
  nfapi_srs_report_tlv_t *report_tlv = &srs_ind->report_tlv;

  switch (srs_ind->srs_usage) {
    case NR_SRS_ResourceSet__usage_beamManagement: {
      nfapi_nr_srs_beamforming_report_t nr_srs_bf_report;
      unpack_nr_srs_beamforming_report(report_tlv->value,
                                       report_tlv->length,
                                       &nr_srs_bf_report,
                                       sizeof(nfapi_nr_srs_beamforming_report_t));

      if (nr_srs_bf_report.wide_band_snr == 0xFF) {
        LOG_W(NR_MAC, "Invalid wide_band_snr for RNTI %04x\n", srs_ind->rnti);
        NR_SCHED_UNLOCK(&nrmac->sched_lock);
        return;
      }

      int wide_band_snr_dB = (nr_srs_bf_report.wide_band_snr >> 1) - 64;

#ifdef SRS_IND_DEBUG
      LOG_I(NR_MAC, "nr_srs_bf_report.prg_size = %i\n", nr_srs_bf_report.prg_size);
      LOG_I(NR_MAC, "nr_srs_bf_report.num_symbols = %i\n", nr_srs_bf_report.num_symbols);
      LOG_I(NR_MAC, "nr_srs_bf_report.wide_band_snr = %i (%i dB)\n", nr_srs_bf_report.wide_band_snr, wide_band_snr_dB);
      LOG_I(NR_MAC, "nr_srs_bf_report.num_reported_symbols = %i\n", nr_srs_bf_report.num_reported_symbols);
      LOG_I(NR_MAC, "nr_srs_bf_report.prgs[0].num_prgs = %i\n", nr_srs_bf_report.prgs[0].num_prgs);
      for (int prg_idx = 0; prg_idx < nr_srs_bf_report.prgs[0].num_prgs; prg_idx++) {
        LOG_I(NR_MAC,
              "nr_srs_bf_report.prgs[0].prg_list[%3i].rb_snr = %i (%i dB)\n",
              prg_idx,
              nr_srs_bf_report.prgs[0].prg_list[prg_idx].rb_snr,
              (nr_srs_bf_report.prgs[0].prg_list[prg_idx].rb_snr >> 1) - 64);
      }
#endif

      sprintf(stats->srs_stats, "UL-SNR %i dB", wide_band_snr_dB);

      const int ul_prbblack_SNR_threshold = nr_mac->ul_prbblack_SNR_threshold;
      uint16_t *ulprbbl = nr_mac->ulprbbl;

      uint16_t num_rbs = nr_srs_bf_report.prg_size * nr_srs_bf_report.reported_symbol_list[0].num_prgs;
      memset(ulprbbl, 0, num_rbs * sizeof(uint16_t));
      for (int rb = 0; rb < num_rbs; rb++) {
        int snr = (nr_srs_bf_report.reported_symbol_list[0].prg_list[rb / nr_srs_bf_report.prg_size].rb_snr >> 1) - 64;
        if (snr < wide_band_snr_dB - ul_prbblack_SNR_threshold) {
          ulprbbl[rb] = 0x3FFF; // all symbols taken
        }
        LOG_D(NR_MAC, "ulprbbl[%3i] = 0x%x\n", rb, ulprbbl[rb]);
      }

      break;
    }

    case NR_SRS_ResourceSet__usage_codebook: {
      nfapi_nr_srs_normalized_channel_iq_matrix_t nr_srs_channel_iq_matrix;
      unpack_nr_srs_normalized_channel_iq_matrix(report_tlv->value,
                                                 report_tlv->length,
                                                 &nr_srs_channel_iq_matrix,
                                                 sizeof(nfapi_nr_srs_normalized_channel_iq_matrix_t));

#ifdef SRS_IND_DEBUG
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.normalized_iq_representation = %i\n", nr_srs_channel_iq_matrix.normalized_iq_representation);
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.num_gnb_antenna_elements = %i\n", nr_srs_channel_iq_matrix.num_gnb_antenna_elements);
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.num_ue_srs_ports = %i\n", nr_srs_channel_iq_matrix.num_ue_srs_ports);
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.prg_size = %i\n", nr_srs_channel_iq_matrix.prg_size);
      LOG_I(NR_MAC, "nr_srs_channel_iq_matrix.num_prgs = %i\n", nr_srs_channel_iq_matrix.num_prgs);
      c16_t *channel_matrix16 = (c16_t *)nr_srs_channel_iq_matrix.channel_matrix;
      c8_t *channel_matrix8 = (c8_t *)nr_srs_channel_iq_matrix.channel_matrix;
      for (int uI = 0; uI < nr_srs_channel_iq_matrix.num_ue_srs_ports; uI++) {
        for (int gI = 0; gI < nr_srs_channel_iq_matrix.num_gnb_antenna_elements; gI++) {
          for (int pI = 0; pI < nr_srs_channel_iq_matrix.num_prgs; pI++) {
            uint16_t index = uI * nr_srs_channel_iq_matrix.num_gnb_antenna_elements * nr_srs_channel_iq_matrix.num_prgs + gI * nr_srs_channel_iq_matrix.num_prgs + pI;
            LOG_I(NR_MAC,
                  "(uI %i, gI %i, pI %i) channel_matrix --> real %i, imag %i\n",
                  uI,
                  gI,
                  pI,
                  nr_srs_channel_iq_matrix.normalized_iq_representation == 0 ? channel_matrix8[index].r : channel_matrix16[index].r,
                  nr_srs_channel_iq_matrix.normalized_iq_representation == 0 ? channel_matrix8[index].i : channel_matrix16[index].i);
          }
        }
      }
#endif

      NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
      NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;
      sched_ctrl->srs_feedback.sri = NR_SRS_SRI_0;

      start_meas(&nr_mac->nr_srs_ri_computation_timer);
      nr_srs_ri_computation(&nr_srs_channel_iq_matrix, current_BWP, &sched_ctrl->srs_feedback.ul_ri);
      stop_meas(&nr_mac->nr_srs_ri_computation_timer);

      start_meas(&nr_mac->nr_srs_tpmi_computation_timer);
      sched_ctrl->srs_feedback.tpmi = nr_srs_tpmi_estimation(current_BWP->pusch_Config,
                                                             current_BWP->transform_precoding,
                                                             nr_srs_channel_iq_matrix.channel_matrix,
                                                             nr_srs_channel_iq_matrix.normalized_iq_representation,
                                                             nr_srs_channel_iq_matrix.num_gnb_antenna_elements,
                                                             nr_srs_channel_iq_matrix.num_ue_srs_ports,
                                                             nr_srs_channel_iq_matrix.prg_size,
                                                             nr_srs_channel_iq_matrix.num_prgs,
                                                             sched_ctrl->srs_feedback.ul_ri);
      stop_meas(&nr_mac->nr_srs_tpmi_computation_timer);

      sprintf(stats->srs_stats, "UL-RI %d, TPMI %d", sched_ctrl->srs_feedback.ul_ri + 1, sched_ctrl->srs_feedback.tpmi);

      break;
    }

    case NR_SRS_ResourceSet__usage_nonCodebook:
    case NR_SRS_ResourceSet__usage_antennaSwitching:
      LOG_W(NR_MAC, "MAC procedures for this SRS usage are not implemented yet!\n");
      break;

    default:
      AssertFatal(1 == 0, "Invalid SRS usage\n");
  }
  NR_SCHED_UNLOCK(&nrmac->sched_lock);
}

long get_K2(NR_PUSCH_TimeDomainResourceAllocationList_t *tdaList,
            int time_domain_assignment,
            int mu,
            const NR_ServingCellConfigCommon_t *scc)
{
  /* we assume that this function is mutex-protected from outside */
  NR_PUSCH_TimeDomainResourceAllocation_t *tda = tdaList->list.array[time_domain_assignment];
  const int NTN_gNB_Koffset = get_NTN_Koffset(scc);

  if (tda->k2)
    return *tda->k2 + NTN_gNB_Koffset;
  else if (mu < 2)
    return 1 + NTN_gNB_Koffset;
  else if (mu == 2)
    return 2 + NTN_gNB_Koffset;
  else
    return 3 + NTN_gNB_Koffset;
}

static bool nr_UE_is_to_be_scheduled(const frame_structure_t *fs,
                                     NR_UE_info_t *UE,
                                     frame_t frame,
                                     sub_frame_t slot,
                                     uint32_t ulsch_max_frame_inactivity)
{
  const int n = fs->numb_slots_frame;
  const int now = frame * n + slot;

  const NR_UE_sched_ctrl_t *sched_ctrl =&UE->UE_sched_ctrl;
  /**
   * Force the default transmission in a full slot as early
   * as possible in the UL portion of TDD period (last_ul_slot) */
  int num_slots_per_period = fs->numb_slots_period;
  int last_ul_slot = fs->frame_type == TDD ? get_first_ul_slot(fs, false) : sched_ctrl->last_ul_slot;
  const int last_ul_sched = sched_ctrl->last_ul_frame * n + last_ul_slot;
  const int diff = (now - last_ul_sched + 1024 * n) % (1024 * n);
  /* UE is to be scheduled if
   * (1) we think the UE has more bytes awaiting than what we scheduled
   * (2) there is a scheduling request
   * (3) or we did not schedule it in more than 10 frames */
  const bool has_data = sched_ctrl->estimated_ul_buffer > sched_ctrl->sched_ul_bytes;
  const bool high_inactivity = diff >= (ulsch_max_frame_inactivity > 0 ? ulsch_max_frame_inactivity * n : num_slots_per_period);
  LOG_D(NR_MAC,
        "%4d.%2d UL inactivity %d slots has_data %d SR %d\n",
        frame,
        slot,
        diff,
        has_data,
        sched_ctrl->SR);
  return has_data || sched_ctrl->SR || high_inactivity;
}

static void update_ul_ue_R_Qm(int mcs, int mcs_table, const NR_PUSCH_Config_t *pusch_Config, uint16_t *R, uint8_t *Qm)
{
  *R = nr_get_code_rate_ul(mcs, mcs_table);
  *Qm = nr_get_Qm_ul(mcs, mcs_table);

  if (pusch_Config && pusch_Config->tp_pi2BPSK && ((mcs_table == 3 && mcs < 2) || (mcs_table == 4 && mcs < 6))) {
    *R >>= 1;
    *Qm <<= 1;
  }
}

static void nr_ue_max_mcs_min_rb(int mu,
                                 int ph_limit,
                                 NR_sched_pusch_t *sched_pusch,
                                 NR_UE_UL_BWP_t *ul_bwp,
                                 uint16_t minRb,
                                 uint32_t tbs,
                                 uint16_t *Rb,
                                 uint8_t *mcs)
{
  AssertFatal(*Rb >= minRb, "illegal Rb %d < minRb %d\n", *Rb, minRb);
  AssertFatal(*mcs >= 0 && *mcs <= 28, "illegal MCS %d\n", *mcs);

  int tbs_bits = tbs << 3;
  uint16_t R;
  uint8_t Qm;
  update_ul_ue_R_Qm(*mcs, ul_bwp->mcs_table, ul_bwp->pusch_Config, &R, &Qm);

  long *deltaMCS = ul_bwp->pusch_Config ? ul_bwp->pusch_Config->pusch_PowerControl->deltaMCS : NULL;
  tbs_bits = nr_compute_tbs(Qm, R, *Rb,
                              sched_pusch->tda_info.nrOfSymbols,
                              sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                              0, // nb_rb_oh
                              0,
                              sched_pusch->nrOfLayers);

  int tx_power = compute_ph_factor(mu,
                                   tbs_bits,
                                   *Rb,
                                   sched_pusch->nrOfLayers,
                                   sched_pusch->tda_info.nrOfSymbols,
                                   sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                                   deltaMCS,
                                   true);

  while (ph_limit < tx_power && *Rb > minRb) {
    (*Rb)--;
    tbs_bits = nr_compute_tbs(Qm, R, *Rb,
                              sched_pusch->tda_info.nrOfSymbols,
                              sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                              0, // nb_rb_oh
                              0,
                              sched_pusch->nrOfLayers);
    tx_power = compute_ph_factor(mu,
                                 tbs_bits,
                                 *Rb,
                                 sched_pusch->nrOfLayers,
                                 sched_pusch->tda_info.nrOfSymbols,
                                 sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                                 deltaMCS,
                                 true);
    LOG_D(NR_MAC, "Checking %d RBs, MCS %d, ph_limit %d, tx_power %d\n",*Rb,*mcs,ph_limit,tx_power);
  }

  while (ph_limit < tx_power && *mcs > 0) {
    (*mcs)--;
    update_ul_ue_R_Qm(*mcs, ul_bwp->mcs_table, ul_bwp->pusch_Config, &R, &Qm);
    tbs_bits = nr_compute_tbs(Qm, R, *Rb,
                              sched_pusch->tda_info.nrOfSymbols,
                              sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                              0, // nb_rb_oh
                              0,
                              sched_pusch->nrOfLayers);
    tx_power = compute_ph_factor(mu,
                                 tbs_bits,
                                 *Rb,
                                 sched_pusch->nrOfLayers,
                                 sched_pusch->tda_info.nrOfSymbols,
                                 sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                                 deltaMCS,
                                 true);
    LOG_D(NR_MAC, "Checking %d RBs, MCS %d, ph_limit %d, tx_power %d\n",*Rb,*mcs,ph_limit,tx_power);
  }

  if (ph_limit < tx_power)
    LOG_D(NR_MAC, "Normalized power %d based on current resources (RBs %d, MCS %d) exceed reported PHR %d (normalized value)\n",
          tx_power, *Rb, *mcs, ph_limit);
}

static bool allocate_ul_retransmission(gNB_MAC_INST *nrmac,
				       frame_t frame,
				       sub_frame_t slot,
				       uint16_t *rballoc_mask,
				       int *n_rb_sched,
				       int dci_beam_idx,
				       NR_UE_info_t* UE,
				       int harq_pid,
				       const NR_ServingCellConfigCommon_t *scc,
				       const int tda)
{
  const int CC_id = 0;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_sched_pusch_t *retInfo = &sched_ctrl->ul_harq_processes[harq_pid].sched_pusch;
  NR_UE_UL_BWP_t *ul_bwp = &UE->current_UL_BWP;

  int rbStart = 0; // wrt BWP start
  const uint32_t bwpSize = ul_bwp->BWPSize;
  const uint32_t bwpStart = ul_bwp->BWPStart;
  const uint8_t nrOfLayers = retInfo->nrOfLayers;
  LOG_D(NR_MAC,"retInfo->time_domain_allocation = %d, tda = %d\n", retInfo->time_domain_allocation, tda);
  LOG_D(NR_MAC,"tbs %d\n",retInfo->tb_size);
  NR_tda_info_t tda_info = get_ul_tda_info(ul_bwp,
                                           sched_ctrl->coreset->controlResourceSetId,
                                           sched_ctrl->search_space->searchSpaceType->present,
                                           TYPE_C_RNTI_,
                                           tda);
  if (!tda_info.valid_tda)
    return false;

  bool reuse_old_tda = (retInfo->tda_info.startSymbolIndex == tda_info.startSymbolIndex) && (retInfo->tda_info.nrOfSymbols <= tda_info.nrOfSymbols);
  if (reuse_old_tda && nrOfLayers == retInfo->nrOfLayers) {
    /* Check the resource is enough for retransmission */
    const uint16_t slbitmap = SL_to_bitmap(retInfo->tda_info.startSymbolIndex, retInfo->tda_info.nrOfSymbols);
    while (rbStart < bwpSize && (rballoc_mask[rbStart + bwpStart] & slbitmap))
      rbStart++;
    if (rbStart + retInfo->rbSize > bwpSize) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not allocate UL retransmission: no resources (rbStart %d, retInfo->rbSize %d, bwpSize %d) \n",
            UE->rnti,
            frame,
            slot,
            rbStart,
            retInfo->rbSize,
            bwpSize);
      return false;
    }
    LOG_D(NR_MAC, "Retransmission keeping TDA %d and TBS %d\n", tda, retInfo->tb_size);
  } else {
    NR_pusch_dmrs_t dmrs_info = get_ul_dmrs_params(scc, ul_bwp, &tda_info, nrOfLayers);
    /* the retransmission will use a different time domain allocation, check
     * that we have enough resources */
    const uint16_t slbitmap = SL_to_bitmap(tda_info.startSymbolIndex, tda_info.nrOfSymbols);
    while (rbStart < bwpSize && (rballoc_mask[rbStart + bwpStart] & slbitmap))
      rbStart++;
    int rbSize = 0;
    while (rbStart + rbSize < bwpSize && !(rballoc_mask[rbStart + bwpStart + rbSize] & slbitmap))
      rbSize++;
    uint32_t new_tbs;
    uint16_t new_rbSize;
    bool success = nr_find_nb_rb(retInfo->Qm,
                                 retInfo->R,
                                 UE->current_UL_BWP.transform_precoding,
                                 nrOfLayers,
                                 tda_info.nrOfSymbols,
                                 dmrs_info.N_PRB_DMRS * dmrs_info.num_dmrs_symb,
                                 retInfo->tb_size,
                                 1, /* minimum of 1RB: need to find exact TBS, don't preclude any number */
                                 rbSize,
                                 &new_tbs,
                                 &new_rbSize);
    if (!success || new_tbs != retInfo->tb_size) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] allocation of UL retransmission failed: new TBsize %d of new TDA does not match old TBS %d \n",
            UE->rnti,
            frame,
            slot,
            new_tbs,
            retInfo->tb_size);
      return false; /* the maximum TBsize we might have is smaller than what we need */
    }
    LOG_D(NR_MAC, "Retransmission with TDA %d->%d and TBS %d -> %d\n", retInfo->time_domain_allocation, tda, retInfo->tb_size, new_tbs);
    /* we can allocate it. Overwrite the time_domain_allocation, the number
     * of RBs, and the new TB size. The rest is done below */
    retInfo->tb_size = new_tbs;
    retInfo->rbSize = new_rbSize;
    retInfo->time_domain_allocation = tda;
    retInfo->dmrs_info = dmrs_info;
    retInfo->tda_info = tda_info;
  }

  /* Find a free CCE */
  int CCEIndex = get_cce_index(nrmac,
                               CC_id,
                               slot,
                               UE->rnti,
                               &sched_ctrl->aggregation_level,
                               dci_beam_idx,
                               sched_ctrl->search_space,
                               sched_ctrl->coreset,
                               &sched_ctrl->sched_pdcch,
                               false,
                               sched_ctrl->pdcch_cl_adjust);
  if (CCEIndex<0) {
    LOG_D(NR_MAC, "[UE %04x][%4d.%2d] no free CCE for retransmission UL DCI UE\n", UE->rnti, frame, slot);
    return false;
  }

  sched_ctrl->cce_index = CCEIndex;
  fill_pdcch_vrb_map(nrmac, CC_id, &sched_ctrl->sched_pdcch, CCEIndex, sched_ctrl->aggregation_level, dci_beam_idx);
  int slots_frame = nrmac->frame_structure.numb_slots_frame;
  retInfo->frame = (frame + (slot + tda_info.k2) / slots_frame) % MAX_FRAME_NUMBER;
  retInfo->slot = (slot + tda_info.k2) % slots_frame;
  /* Get previous PSUCH field info */
  sched_ctrl->sched_pusch = *retInfo;
  NR_sched_pusch_t *sched_pusch = &sched_ctrl->sched_pusch;

  LOG_D(NR_MAC,
        "%4d.%2d Allocate UL retransmission RNTI %04x sched %4d.%2d (%d RBs)\n",
        frame,
        slot,
        UE->rnti,
        sched_pusch->frame,
        sched_pusch->slot,
        sched_pusch->rbSize);

  sched_pusch->rbStart = rbStart;
  /* no need to recompute the TBS, it will be the same */

  /* Mark the corresponding RBs as used */
  n_rb_sched -= sched_pusch->rbSize;
  for (int rb = bwpStart; rb < sched_ctrl->sched_pusch.rbSize; rb++)
    rballoc_mask[rb + sched_ctrl->sched_pusch.rbStart] |= SL_to_bitmap(sched_pusch->tda_info.startSymbolIndex, sched_pusch->tda_info.nrOfSymbols);
  return true;
}

static uint32_t ul_pf_tbs[5][29]; // pre-computed, approximate TBS values for PF coefficient
typedef struct UEsched_s {
  float coef;
  NR_UE_info_t * UE;
} UEsched_t;

static int comparator(const void *p, const void *q) {
  return ((UEsched_t*)p)->coef < ((UEsched_t*)q)->coef;
}

static void pf_ul(module_id_t module_id,
                  frame_t frame,
                  int slot,
                  frame_t sched_frame,
                  int sched_slot,
                  NR_UE_info_t *UE_list[],
                  int max_num_ue,
                  int num_beams,
                  int n_rb_sched[num_beams])
{
  const int CC_id = 0;
  gNB_MAC_INST *nrmac = RC.nrmac[module_id];
  NR_ServingCellConfigCommon_t *scc = nrmac->common_channels[CC_id].ServingCellConfigCommon;
  int slots_per_frame = nrmac->frame_structure.numb_slots_frame;
  const int min_rb = nrmac->min_grant_prb;
  // UEs that could be scheduled
  UEsched_t UE_sched[MAX_MOBILES_PER_GNB + 1] = {0};
  int remainUEs[num_beams];
  for (int i = 0; i < num_beams; i++)
    remainUEs[i] = max_num_ue;
  int curUE = 0;

  /* Loop UE_list to calculate throughput and coeff */
  UE_iterator(UE_list, UE) {

    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    if (!UE->Msg4_MsgB_ACKed || sched_ctrl->ul_failure)
      continue;

    LOG_D(NR_MAC,"pf_ul: preparing UL scheduling for UE %04x\n",UE->rnti);
    NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;

    NR_sched_pusch_t *sched_pusch = &sched_ctrl->sched_pusch;
    const NR_mac_dir_stats_t *stats = &UE->mac_stats.ul;

    /* Calculate throughput */
    const float a = 0.01f;
    const uint32_t b = stats->current_bytes;
    UE->ul_thr_ue = (1 - a) * UE->ul_thr_ue + a * b;

    int total_rem_ues = 0;
    for (int i = 0; i < num_beams; i++)
      total_rem_ues += remainUEs[i];
    if (total_rem_ues == 0)
      continue;

    NR_beam_alloc_t dci_beam = beam_allocation_procedure(&nrmac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame);
    if (dci_beam.idx < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] Beam could not be allocated\n", UE->rnti, frame, slot);
      continue;
    }

    NR_beam_alloc_t beam = beam_allocation_procedure(&nrmac->beam_info, sched_frame, sched_slot, UE->UE_beam_index, slots_per_frame);
    if (beam.idx < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] Beam could not be allocated\n", UE->rnti, frame, slot);
      reset_beam_status(&nrmac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame, dci_beam.new_beam);
      continue;
    }
    const int index = ul_buffer_index(sched_frame, sched_slot, slots_per_frame, nrmac->vrb_map_UL_size);
    uint16_t *rballoc_mask = &nrmac->common_channels[CC_id].vrb_map_UL[beam.idx][index * MAX_BWP_SIZE];

    /* Check if retransmission is necessary */
    sched_pusch->ul_harq_pid = sched_ctrl->retrans_ul_harq.head;
    LOG_D(NR_MAC,"pf_ul: UE %04x harq_pid %d\n", UE->rnti, sched_pusch->ul_harq_pid);
    if (sched_pusch->ul_harq_pid >= 0) {
      /* Allocate retransmission*/
      const int tda = get_ul_tda(nrmac, sched_frame, sched_slot);
      bool r = allocate_ul_retransmission(nrmac,
                                          frame,
                                          slot,
                                          rballoc_mask,
                                          &n_rb_sched[beam.idx],
                                          dci_beam.idx,
                                          UE,
                                          sched_pusch->ul_harq_pid,
                                          scc,
                                          tda);
      if (!r) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] UL retransmission could not be allocated\n", UE->rnti, frame, slot);
        reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, UE->UE_beam_index, slots_per_frame, beam.new_beam);
        reset_beam_status(&nrmac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame, dci_beam.new_beam);
        continue;
      }
      else
        LOG_D(NR_MAC,"%4d.%2d UL Retransmission UE RNTI %04x to be allocated, max_num_ue %d\n", frame, slot, UE->rnti,max_num_ue);

      /* reduce max_num_ue once we are sure UE can be allocated, i.e., has CCE */
      remainUEs[beam.idx]--;
      continue;
    }

    /* skip this UE if there are no free HARQ processes. This can happen e.g.
     * if the UE disconnected in L2sim, in which case the gNB is not notified
     * (this can be considered a design flaw) */
    if (sched_ctrl->available_ul_harq.head < 0) {
      reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, UE->UE_beam_index, slots_per_frame, beam.new_beam);
      reset_beam_status(&nrmac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame, dci_beam.new_beam);
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] has no free UL HARQ process, skipping\n", UE->rnti, frame, slot);
      continue;
    }

    const int B = max(0, sched_ctrl->estimated_ul_buffer - sched_ctrl->sched_ul_bytes);
    /* preprocessor computed sched_frame/sched_slot */
    const bool do_sched = nr_UE_is_to_be_scheduled(&nrmac->frame_structure,
                                                   UE,
                                                   sched_frame,
                                                   sched_slot,
                                                   nrmac->ulsch_max_frame_inactivity);

    LOG_D(NR_MAC,"pf_ul: do_sched UE %04x => %s\n", UE->rnti, do_sched ? "yes" : "no");
    if ((B == 0 && !do_sched) || nr_timer_is_active(&sched_ctrl->transm_interrupt)) {
      reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, UE->UE_beam_index, slots_per_frame, beam.new_beam);
      reset_beam_status(&nrmac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame, dci_beam.new_beam);
      continue;
    }

    const NR_bler_options_t *bo = &nrmac->ul_bler;
    const int max_mcs_table = (current_BWP->mcs_table == 0 || current_BWP->mcs_table == 2) ? 28 : 27;
    const int max_mcs = min(bo->max_mcs, max_mcs_table); /* no per-user maximum MCS yet */
    if (bo->harq_round_max == 1) {
      sched_pusch->mcs = max_mcs;
      sched_ctrl->ul_bler_stats.mcs = sched_pusch->mcs;
    } else {
      sched_pusch->mcs = get_mcs_from_bler(bo, stats, &sched_ctrl->ul_bler_stats, max_mcs, frame);
      LOG_D(NR_MAC,"%d.%d starting mcs %d bleri %f\n", frame, slot, sched_pusch->mcs, sched_ctrl->ul_bler_stats.bler);
    }
    /* Schedule UE on SR or UL inactivity and no data (otherwise, will be scheduled
     * based on data to transmit) */
    if (B == 0 && do_sched) {
      /* if no data, pre-allocate 5RB */
      /* Find a free CCE */
      int CCEIndex = get_cce_index(nrmac,
                                   CC_id, slot, UE->rnti,
                                   &sched_ctrl->aggregation_level,
                                   dci_beam.idx,
                                   sched_ctrl->search_space,
                                   sched_ctrl->coreset,
                                   &sched_ctrl->sched_pdcch,
                                   false,
                                   sched_ctrl->pdcch_cl_adjust);
      if (CCEIndex < 0) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] no free CCE for UL DCI (BSR 0)\n", UE->rnti, frame, slot);
        reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, UE->UE_beam_index, slots_per_frame, beam.new_beam);
        reset_beam_status(&nrmac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame, dci_beam.new_beam);
        continue;
      }

      sched_pusch->nrOfLayers = sched_ctrl->srs_feedback.ul_ri + 1;
      sched_pusch->time_domain_allocation = get_ul_tda(nrmac, sched_frame, sched_slot);
      sched_pusch->tda_info = get_ul_tda_info(current_BWP,
                                              sched_ctrl->coreset->controlResourceSetId,
                                              sched_ctrl->search_space->searchSpaceType->present,
                                              TYPE_C_RNTI_,
                                              sched_pusch->time_domain_allocation);
      AssertFatal(sched_pusch->tda_info.valid_tda, "Invalid TDA from get_ul_tda_info\n");
      sched_pusch->dmrs_info = get_ul_dmrs_params(scc, current_BWP, &sched_pusch->tda_info, sched_pusch->nrOfLayers);

      int rbStart = 0; // wrt BWP start
      LOG_D(NR_MAC,
            "Looking for min_rb %d RBs, starting at %d num_dmrs_cdm_grps_no_data %d\n",
            min_rb,
            rbStart,
            sched_pusch->dmrs_info.num_dmrs_cdm_grps_no_data);
      const uint32_t bwpSize = current_BWP->BWPSize;
      const uint32_t bwpStart = current_BWP->BWPStart;
      const uint16_t slbitmap = SL_to_bitmap(sched_pusch->tda_info.startSymbolIndex, sched_pusch->tda_info.nrOfSymbols);
      while (rbStart < bwpSize && (rballoc_mask[rbStart + bwpStart] & slbitmap))
        rbStart++;
      if (rbStart + min_rb >= bwpSize) {
        LOG_D(NR_MAC,
              "[UE %04x][%4d.%2d] could not allocate continuous UL data: no resources (rbStart %d, min_rb %d, bwpSize %d)\n",
              UE->rnti,
              frame,
              slot,
              rbStart,
              min_rb,
              bwpSize);
        reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, UE->UE_beam_index, slots_per_frame, beam.new_beam);
        reset_beam_status(&nrmac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame, dci_beam.new_beam);
        continue;
      }

      sched_ctrl->cce_index = CCEIndex;
      fill_pdcch_vrb_map(nrmac, CC_id, &sched_ctrl->sched_pdcch, CCEIndex, sched_ctrl->aggregation_level, dci_beam.idx);

      NR_sched_pusch_t *sched_pusch = &sched_ctrl->sched_pusch;
      sched_pusch->mcs = min(nrmac->min_grant_mcs, sched_pusch->mcs);
      update_ul_ue_R_Qm(sched_pusch->mcs, current_BWP->mcs_table, current_BWP->pusch_Config, &sched_pusch->R, &sched_pusch->Qm);
      sched_pusch->rbStart = rbStart;
      sched_pusch->rbSize = min_rb;
      sched_pusch->frame = sched_frame;
      sched_pusch->slot = sched_slot;
      sched_pusch->tb_size = nr_compute_tbs(sched_pusch->Qm,
                                            sched_pusch->R,
                                            sched_pusch->rbSize,
                                            sched_pusch->tda_info.nrOfSymbols,
                                            sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                                            0, // nb_rb_oh
                                            0,
                                            sched_pusch->nrOfLayers) >> 3;
      long *deltaMCS = current_BWP->pusch_Config ? current_BWP->pusch_Config->pusch_PowerControl->deltaMCS : NULL;

      sched_pusch->phr_txpower_calc = compute_ph_factor(current_BWP->scs,
                                                        sched_pusch->tb_size << 3,
                                                        sched_pusch->rbSize,
                                                        sched_pusch->nrOfLayers,
                                                        sched_pusch->tda_info.nrOfSymbols,
                                                        sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                                                        deltaMCS,
                                                        false);
      LOG_D(NR_MAC,
            "pf_ul %d.%d UE %x Scheduling PUSCH (no data) nrb %d mcs %d tbs %d bits phr_txpower %d\n",
            frame,
            slot,
            UE->rnti,
            sched_pusch->rbSize,
            sched_pusch->mcs,
            sched_pusch->tb_size << 3,
            sched_pusch->phr_txpower_calc);

      /* Mark the corresponding RBs as used */
      n_rb_sched[beam.idx] -= sched_pusch->rbSize;
      for (int rb = bwpStart; rb < sched_ctrl->sched_pusch.rbSize; rb++)
        rballoc_mask[rb + sched_ctrl->sched_pusch.rbStart] |= slbitmap;

      remainUEs[beam.idx]--;
      continue;
    }

    /* Create UE_sched for UEs eligibale for new data transmission*/
    /* Calculate coefficient*/
    const uint32_t tbs = ul_pf_tbs[current_BWP->mcs_table][sched_pusch->mcs];
    float coeff_ue = (float) tbs / UE->ul_thr_ue;
    LOG_D(NR_MAC, "[UE %04x][%4d.%2d] b %d, ul_thr_ue %f, tbs %d, coeff_ue %f\n",
          UE->rnti,
          frame,
          slot,
          b,
          UE->ul_thr_ue,
          tbs,
          coeff_ue);
    UE_sched[curUE].coef = coeff_ue;
    UE_sched[curUE].UE = UE;
    curUE++;
  }

  qsort(UE_sched, sizeofArray(UE_sched), sizeof(UEsched_t), comparator);
  UEsched_t *iterator=UE_sched;

  /* Loop UE_sched to find max coeff and allocate transmission */
  while (iterator->UE != NULL) {
    NR_UE_UL_BWP_t *current_BWP = &iterator->UE->current_UL_BWP;
    NR_UE_sched_ctrl_t *sched_ctrl = &iterator->UE->UE_sched_ctrl;
    NR_sched_pusch_t *sched_pusch = &sched_ctrl->sched_pusch;

    NR_beam_alloc_t beam = beam_allocation_procedure(&nrmac->beam_info, sched_frame, sched_slot, iterator->UE->UE_beam_index, slots_per_frame);
    if (beam.idx < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] Beam could not be allocated\n", iterator->UE->rnti, frame, slot);
      iterator++;
      continue;
    }

    if (remainUEs[beam.idx] == 0 || n_rb_sched[beam.idx] < min_rb) {
      reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
      iterator++;
      continue;
    }

    NR_beam_alloc_t dci_beam = beam_allocation_procedure(&nrmac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame);
    if (dci_beam.idx < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] Beam could not be allocated\n", iterator->UE->rnti, frame, slot);
      reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
      iterator++;
      continue;
    }

    int CCEIndex = get_cce_index(nrmac,
                                 CC_id, slot, iterator->UE->rnti,
                                 &sched_ctrl->aggregation_level,
                                 dci_beam.idx,
                                 sched_ctrl->search_space,
                                 sched_ctrl->coreset,
                                 &sched_ctrl->sched_pdcch,
                                 false,
                                 sched_ctrl->pdcch_cl_adjust);

    if (CCEIndex < 0) {
      reset_beam_status(&nrmac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame, dci_beam.new_beam);
      reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] no free CCE for UL DCI\n", iterator->UE->rnti, frame, slot);
      iterator++;
      continue;
    }
    else
      LOG_D(NR_MAC, "%4d.%2d free CCE for UL DCI UE %04x\n", frame, slot, iterator->UE->rnti);



    sched_pusch->nrOfLayers = sched_ctrl->srs_feedback.ul_ri + 1;
    sched_pusch->time_domain_allocation = get_ul_tda(nrmac, sched_frame, sched_slot);
    sched_pusch->tda_info = get_ul_tda_info(current_BWP,
                                            sched_ctrl->coreset->controlResourceSetId,
                                            sched_ctrl->search_space->searchSpaceType->present,
                                            TYPE_C_RNTI_,
                                            sched_pusch->time_domain_allocation);
    AssertFatal(sched_pusch->tda_info.valid_tda, "Invalid TDA from get_ul_tda_info\n");
    sched_pusch->dmrs_info = get_ul_dmrs_params(scc, current_BWP, &sched_pusch->tda_info, sched_pusch->nrOfLayers);

    const int index = ul_buffer_index(sched_frame, sched_slot, slots_per_frame, nrmac->vrb_map_UL_size);
    uint16_t *rballoc_mask = &nrmac->common_channels[CC_id].vrb_map_UL[beam.idx][index * MAX_BWP_SIZE];

    int rbStart = 0;
    const uint16_t slbitmap = SL_to_bitmap(sched_pusch->tda_info.startSymbolIndex, sched_pusch->tda_info.nrOfSymbols);
    const uint32_t bwpSize = current_BWP->BWPSize;
    const uint32_t bwpStart = current_BWP->BWPStart;
    while (rbStart < bwpSize && (rballoc_mask[rbStart + bwpStart] & slbitmap))
      rbStart++;
    sched_pusch->rbStart = rbStart;
    uint16_t max_rbSize = 1;
    while (rbStart + max_rbSize < bwpSize && !(rballoc_mask[rbStart + bwpStart + max_rbSize] & slbitmap))
      max_rbSize++;

    if (rbStart + min_rb >= bwpSize || max_rbSize < min_rb) {
      reset_beam_status(&nrmac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame, dci_beam.new_beam);
      reset_beam_status(&nrmac->beam_info, sched_frame, sched_slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not allocate UL data: no resources (rbStart %d, min_rb %d, bwpSize %d)\n",
            iterator->UE->rnti,
            frame,
            slot,
            rbStart,
            min_rb,
            bwpSize);
      iterator++;
      continue;
    } else
      LOG_D(NR_MAC,
            "allocating UL data for RNTI %04x (rbStart %d, min_rb %d, max_rbSize %d, bwpSize %d)\n",
            iterator->UE->rnti,
            rbStart,
            min_rb,
            max_rbSize,
            bwpSize);

    /* Calculate the current scheduling bytes */
    const int B = cmax(sched_ctrl->estimated_ul_buffer - sched_ctrl->sched_ul_bytes, 0);
    /* adjust rbSize and MCS according to PHR and BPRE */
    if(sched_ctrl->pcmax != 0 || sched_ctrl->ph != 0) // verify if the PHR related parameter have been initialized
      nr_ue_max_mcs_min_rb(current_BWP->scs, sched_ctrl->ph, sched_pusch, current_BWP, min_rb, B, &max_rbSize, &sched_pusch->mcs);

    if (sched_pusch->mcs < sched_ctrl->ul_bler_stats.mcs)
      sched_ctrl->ul_bler_stats.mcs = sched_pusch->mcs; /* force estimated MCS down */

    update_ul_ue_R_Qm(sched_pusch->mcs, current_BWP->mcs_table, current_BWP->pusch_Config, &sched_pusch->R, &sched_pusch->Qm);
    uint16_t rbSize = 0;
    uint32_t TBS = 0;
    nr_find_nb_rb(sched_pusch->Qm,
                  sched_pusch->R,
                  current_BWP->transform_precoding,
                  sched_pusch->nrOfLayers,
                  sched_pusch->tda_info.nrOfSymbols,
                  sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                  B,
                  min_rb,
                  max_rbSize,
                  &TBS,
                  &rbSize);

    // Calacualte the normalized tx_power for PHR
    long *deltaMCS = current_BWP->pusch_Config ? current_BWP->pusch_Config->pusch_PowerControl->deltaMCS : NULL;
    int tbs_bits = TBS << 3;

    sched_pusch->phr_txpower_calc = compute_ph_factor(current_BWP->scs,
                                                      tbs_bits,
                                                      rbSize,
                                                      sched_pusch->nrOfLayers,
                                                      sched_pusch->tda_info.nrOfSymbols,
                                                      sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                                                      deltaMCS,
                                                      false);

    sched_pusch->rbSize = rbSize;
    sched_pusch->tb_size = TBS;
    sched_pusch->frame = sched_frame;
    sched_pusch->slot = sched_slot;
    LOG_D(NR_MAC,
          "rbSize %d (max_rbSize %d), TBS %d, est buf %d, sched_ul %d, B %d, CCE %d, num_dmrs_symb %d, N_PRB_DMRS %d\n",
          rbSize,
          max_rbSize,
          sched_pusch->tb_size,
          sched_ctrl->estimated_ul_buffer,
          sched_ctrl->sched_ul_bytes,
          B,
          sched_ctrl->cce_index,
          sched_pusch->dmrs_info.num_dmrs_symb,
          sched_pusch->dmrs_info.N_PRB_DMRS);

    /* Mark the corresponding RBs as used */

    sched_ctrl->cce_index = CCEIndex;
    fill_pdcch_vrb_map(nrmac, CC_id, &sched_ctrl->sched_pdcch, CCEIndex, sched_ctrl->aggregation_level, dci_beam.idx);

    n_rb_sched[beam.idx] -= sched_pusch->rbSize;
    for (int rb = bwpStart; rb < sched_ctrl->sched_pusch.rbSize; rb++)
      rballoc_mask[rb + sched_ctrl->sched_pusch.rbStart] |= slbitmap;

    /* reduce max_num_ue once we are sure UE can be allocated, i.e., has CCE */
    remainUEs[beam.idx]--;
    iterator++;
  }
}

static bool nr_ulsch_preprocessor(module_id_t module_id, frame_t frame, sub_frame_t slot)
{
  gNB_MAC_INST *nr_mac = RC.nrmac[module_id];
  // no UEs
  if (nr_mac->UE_info.list[0] == NULL)
    return false;

  NR_COMMON_channels_t *cc = nr_mac->common_channels;
  NR_ServingCellConfigCommon_t *scc = cc->ServingCellConfigCommon;
  const NR_SIB1_t *sib1 = nr_mac->common_channels[0].sib1 ? nr_mac->common_channels[0].sib1->message.choice.c1->choice.systemInformationBlockType1 : NULL;
  NR_ServingCellConfigCommonSIB_t *scc_sib1 = sib1 ? sib1->servingCellConfigCommon : NULL;
  AssertFatal(scc || scc_sib1, "We need one serving cell config common\n");
  const int slots_frame = nr_mac->frame_structure.numb_slots_frame;
  // TODO we assume the same K2 for all UEs
  const int K2 = nr_mac->radio_config.minRXTXTIME + get_NTN_Koffset(scc);
  const int sched_frame = (frame + (slot + K2) / slots_frame) % MAX_FRAME_NUMBER;
  const int sched_slot = (slot + K2) % slots_frame;
  if (!is_ul_slot(sched_slot, &nr_mac->frame_structure))
    return false;

  int num_beams = nr_mac->beam_info.beam_allocation ? nr_mac->beam_info.beams_per_period : 1;
  int bw = scc->uplinkConfigCommon->frequencyInfoUL->scs_SpecificCarrierList.list.array[0]->carrierBandwidth;
  int len[num_beams];
  for (int i = 0; i < num_beams; i++)
    len[i] = bw;

  int average_agg_level = 4; // TODO find a better estimation
  int max_sched_ues = bw / (average_agg_level * NR_NB_REG_PER_CCE);

  // FAPI cannot handle more than MAX_DCI_CORESET DCIs
  max_sched_ues = min(max_sched_ues, MAX_DCI_CORESET);

  /* proportional fair scheduling algorithm */
  pf_ul(module_id, frame, slot, sched_frame, sched_slot, nr_mac->UE_info.list, max_sched_ues, num_beams, len);
  return true;
}

nr_pp_impl_ul nr_init_ulsch_preprocessor(int CC_id)
{
  /* during initialization: no mutex needed */
  /* in the PF algorithm, we have to use the TBsize to compute the coefficient.
   * This would include the number of DMRS symbols, which in turn depends on
   * the time domain allocation. In case we are in a mixed slot, we do not want
   * to recalculate all these values, and therefore we provide a look-up table
   * which should approximately(!) give us the TBsize. In particular, the
   * number of symbols, the number of DMRS symbols, and the exact Qm and R, are
   * not correct*/
  for (int mcsTableIdx = 0; mcsTableIdx < 5; ++mcsTableIdx) {
    for (int mcs = 0; mcs < 29; ++mcs) {
      if (mcs > 27 && (mcsTableIdx == 1 || mcsTableIdx == 3 || mcsTableIdx == 4))
        continue;
      const uint8_t Qm = nr_get_Qm_ul(mcs, mcsTableIdx);
      const uint16_t R = nr_get_code_rate_ul(mcs, mcsTableIdx);
      /* note: we do not update R/Qm based on low MCS or pi2BPSK */
      ul_pf_tbs[mcsTableIdx][mcs] = nr_compute_tbs(Qm,
                                                   R,
                                                   1, /* rbSize */
                                                   10, /* hypothetical number of slots */
                                                   0, /* N_PRB_DMRS * N_DMRS_SLOT */
                                                   0 /* N_PRB_oh, 0 for initialBWP */,
                                                   0 /* tb_scaling */,
                                                   1 /* nrOfLayers */)
                                    >> 3;
    }
  }
  return nr_ulsch_preprocessor;
}

void nr_schedule_ulsch(module_id_t module_id, frame_t frame, sub_frame_t slot, nfapi_nr_ul_dci_request_t *ul_dci_req)
{
  gNB_MAC_INST *nr_mac = RC.nrmac[module_id];
  /* already mutex protected: held in gNB_dlsch_ulsch_scheduler() */
  NR_SCHED_ENSURE_LOCKED(&nr_mac->sched_lock);

  /* Uplink data ONLY can be scheduled when the current slot is downlink slot,
   * because we have to schedule the DCI0 first before schedule uplink data */
  if (!is_dl_slot(slot, &nr_mac->frame_structure)) {
    LOG_D(NR_MAC, "Current slot %d is NOT DL slot, cannot schedule DCI0 for UL data\n", slot);
    return;
  }
  bool do_sched = nr_mac->pre_processor_ul(module_id, frame, slot);
  if (!do_sched)
    return;

  ul_dci_req->SFN = frame;
  ul_dci_req->Slot = slot;
  /* a PDCCH PDU groups DCIs per BWP and CORESET. Save a pointer to each
   * allocated PDCCH so we can easily allocate UE's DCIs independent of any
   * CORESET order */
  nfapi_nr_dl_tti_pdcch_pdu_rel15_t *pdcch_pdu_coreset[MAX_NUM_CORESET] = {0};


  NR_ServingCellConfigCommon_t *scc = nr_mac->common_channels[0].ServingCellConfigCommon;
  NR_UEs_t *UE_info = &nr_mac->UE_info;
  UE_iterator( UE_info->list, UE) {
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    if (sched_ctrl->ul_failure && !get_softmodem_params()->phy_test)
      continue;

    NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;

    UE->mac_stats.ul.current_bytes = 0;
    UE->mac_stats.ul.current_rbs = 0;

    /* dynamic PUSCH values (RB alloc, MCS, hence R, Qm, TBS) that change in
     * every TTI are pre-populated by the preprocessor and used below */
    NR_sched_pusch_t *sched_pusch = &sched_ctrl->sched_pusch;
    LOG_D(NR_MAC,"UE %04x : sched_pusch->rbSize %d\n",UE->rnti,sched_pusch->rbSize);
    if (sched_pusch->rbSize <= 0)
      continue;

    uint16_t rnti = UE->rnti;
    sched_ctrl->SR = false;
    int *tpmi = NULL;

    int8_t harq_id = sched_pusch->ul_harq_pid;
    if (harq_id < 0) {
      /* PP has not selected a specific HARQ Process, get a new one */
      harq_id = sched_ctrl->available_ul_harq.head;
      AssertFatal(harq_id >= 0,
                  "no free HARQ process available for UE %04x\n",
                  UE->rnti);
      remove_front_nr_list(&sched_ctrl->available_ul_harq);
      sched_pusch->ul_harq_pid = harq_id;
    } else {
      /* PP selected a specific HARQ process. Check whether it will be a new
       * transmission or a retransmission, and remove from the corresponding
       * list */
      if (sched_ctrl->ul_harq_processes[harq_id].round == 0)
        remove_nr_list(&sched_ctrl->available_ul_harq, harq_id);
      else
        remove_nr_list(&sched_ctrl->retrans_ul_harq, harq_id);
    }
    NR_UE_ul_harq_t *cur_harq = &sched_ctrl->ul_harq_processes[harq_id];
    DevAssert(!cur_harq->is_waiting);
    if (nr_mac->radio_config.disable_harq) {
      finish_nr_ul_harq(sched_ctrl, harq_id);
    } else {
      add_tail_nr_list(&sched_ctrl->feedback_ul_harq, harq_id);
      cur_harq->feedback_slot = sched_pusch->slot;
      cur_harq->is_waiting = true;
    }

    /* Statistics */
    AssertFatal(cur_harq->round < nr_mac->ul_bler.harq_round_max, "Indexing ulsch_rounds[%d] is out of bounds\n", cur_harq->round);
    UE->mac_stats.ul.rounds[cur_harq->round]++;
    if (cur_harq->round == 0) {
      UE->mac_stats.ulsch_total_bytes_scheduled += sched_pusch->tb_size;
      /* Save information on MCS, TBS etc for the current initial transmission
       * so we have access to it when retransmitting */
      cur_harq->sched_pusch = *sched_pusch;
      /* save which time allocation and nrOfLayers have been used, to be used on
       * retransmissions */
      cur_harq->sched_pusch.time_domain_allocation = sched_pusch->time_domain_allocation;
      cur_harq->sched_pusch.nrOfLayers = sched_pusch->nrOfLayers;
      cur_harq->sched_pusch.tpmi = sched_pusch->tpmi;
      sched_ctrl->sched_ul_bytes += sched_pusch->tb_size;
      UE->mac_stats.ul.total_rbs += sched_pusch->rbSize;

    } else {
      LOG_D(NR_MAC,
            "%d.%2d UL retransmission RNTI %04x sched %d.%2d HARQ PID %d round %d NDI %d\n",
            frame,
            slot,
            rnti,
            sched_pusch->frame,
            sched_pusch->slot,
            harq_id,
            cur_harq->round,
            cur_harq->ndi);
      UE->mac_stats.ul.total_rbs_retx += sched_pusch->rbSize;
    }
    UE->mac_stats.ul.current_bytes = sched_pusch->tb_size;
    UE->mac_stats.ul.current_rbs = sched_pusch->rbSize;
    sched_ctrl->last_ul_frame = sched_pusch->frame;
    sched_ctrl->last_ul_slot = sched_pusch->slot;

    LOG_D(NR_MAC,
          "ULSCH/PUSCH: %4d.%2d RNTI %04x UL sched %4d.%2d DCI L %d start %2d RBS %3d startSymbol %2d nb_symbol %2d dmrs_pos %x MCS Table %2d MCS %2d nrOfLayers %2d num_dmrs_cdm_grps_no_data %2d TBS %4d HARQ PID %2d round %d RV %d NDI %d est %6d sched %6d est BSR %6d TPC %d\n",
          frame,
          slot,
          rnti,
          sched_pusch->frame,
          sched_pusch->slot,
          sched_ctrl->aggregation_level,
          sched_pusch->rbStart,
          sched_pusch->rbSize,
          sched_pusch->tda_info.startSymbolIndex,
          sched_pusch->tda_info.nrOfSymbols,
          sched_pusch->dmrs_info.ul_dmrs_symb_pos,
          current_BWP->mcs_table,
          sched_pusch->mcs,
          sched_pusch->nrOfLayers,
          sched_pusch->dmrs_info.num_dmrs_cdm_grps_no_data,
          sched_pusch->tb_size,
          harq_id,
          cur_harq->round,
          nr_rv_round_map[cur_harq->round%4],
          cur_harq->ndi,
          sched_ctrl->estimated_ul_buffer,
          sched_ctrl->sched_ul_bytes,
          sched_ctrl->estimated_ul_buffer - sched_ctrl->sched_ul_bytes,
          sched_ctrl->tpc0);

    /* PUSCH in a later slot, but corresponding DCI now! */
    const int index = ul_buffer_index(sched_pusch->frame,
                                      sched_pusch->slot,
                                      nr_mac->frame_structure.numb_slots_frame,
                                      nr_mac->UL_tti_req_ahead_size);
    nfapi_nr_ul_tti_request_t *future_ul_tti_req = &nr_mac->UL_tti_req_ahead[0][index];
    if (future_ul_tti_req->SFN != sched_pusch->frame || future_ul_tti_req->Slot != sched_pusch->slot)
      LOG_W(NR_MAC,
            "%d.%d future UL_tti_req's frame.slot %d.%d does not match PUSCH %d.%d\n",
            frame, slot,
            future_ul_tti_req->SFN,
            future_ul_tti_req->Slot,
            sched_pusch->frame,
            sched_pusch->slot);
    AssertFatal(future_ul_tti_req->n_pdus <
                sizeof(future_ul_tti_req->pdus_list) / sizeof(future_ul_tti_req->pdus_list[0]),
                "Invalid future_ul_tti_req->n_pdus %d\n", future_ul_tti_req->n_pdus);

    future_ul_tti_req->pdus_list[future_ul_tti_req->n_pdus].pdu_type = NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE;
    future_ul_tti_req->pdus_list[future_ul_tti_req->n_pdus].pdu_size = sizeof(nfapi_nr_pusch_pdu_t);
    nfapi_nr_pusch_pdu_t *pusch_pdu = &future_ul_tti_req->pdus_list[future_ul_tti_req->n_pdus].pusch_pdu;
    memset(pusch_pdu, 0, sizeof(nfapi_nr_pusch_pdu_t));
    future_ul_tti_req->n_pdus += 1;

    LOG_D(NR_MAC,
          "%4d.%2d Scheduling UE specific PUSCH for sched %d.%d, ul_tti_req %d.%d\n",
          frame,
          slot,
          sched_pusch->frame,
          sched_pusch->slot,
          future_ul_tti_req->SFN,
          future_ul_tti_req->Slot);

    pusch_pdu->pdu_bit_map = PUSCH_PDU_BITMAP_PUSCH_DATA;
    pusch_pdu->rnti = rnti;
    pusch_pdu->handle = 0; //not yet used

    /* FAPI: BWP */

    pusch_pdu->bwp_size  = current_BWP->BWPSize;
    pusch_pdu->bwp_start = current_BWP->BWPStart;
    pusch_pdu->subcarrier_spacing = current_BWP->scs;
    pusch_pdu->cyclic_prefix = 0;

    /* FAPI: PUSCH information always included */
    pusch_pdu->target_code_rate = sched_pusch->R;
    pusch_pdu->qam_mod_order = sched_pusch->Qm;
    pusch_pdu->mcs_index = sched_pusch->mcs;
    pusch_pdu->mcs_table = current_BWP->mcs_table;
    pusch_pdu->transform_precoding = current_BWP->transform_precoding;
    if (current_BWP->pusch_Config && current_BWP->pusch_Config->dataScramblingIdentityPUSCH)
      pusch_pdu->data_scrambling_id = *current_BWP->pusch_Config->dataScramblingIdentityPUSCH;
    else
      pusch_pdu->data_scrambling_id = *scc->physCellId;
    pusch_pdu->nrOfLayers = sched_pusch->nrOfLayers;
    // If nrOfLayers is the same as in srs_feedback, we use the best TPMI, i.e. the one in srs_feedback.
    // Otherwise, we use the valid TPMI that we saved in the first transmission.
    if (pusch_pdu->nrOfLayers != (sched_ctrl->srs_feedback.ul_ri + 1))
      tpmi = &sched_pusch->tpmi;
    pusch_pdu->num_dmrs_cdm_grps_no_data = sched_pusch->dmrs_info.num_dmrs_cdm_grps_no_data;

    /* FAPI: DMRS */
    pusch_pdu->num_dmrs_cdm_grps_no_data = sched_pusch->dmrs_info.num_dmrs_cdm_grps_no_data;
    pusch_pdu->dmrs_ports = ((1<<sched_pusch->nrOfLayers) - 1);
    pusch_pdu->ul_dmrs_symb_pos = sched_pusch->dmrs_info.ul_dmrs_symb_pos;
    pusch_pdu->dmrs_config_type = sched_pusch->dmrs_info.dmrs_config_type;
    pusch_pdu->scid = 0;      // DMRS sequence initialization [TS38.211, sec 6.4.1.1.1]
    const NR_DMRS_UplinkConfig_t *NR_DMRS_UplinkConfig = get_DMRS_UplinkConfig(current_BWP->pusch_Config, &sched_pusch->tda_info);
    if (pusch_pdu->transform_precoding) { // transform precoding disabled
      long *scramblingid=NULL;
      pusch_pdu->pusch_identity = *scc->physCellId;
      if (NR_DMRS_UplinkConfig && pusch_pdu->scid == 0)
        scramblingid = NR_DMRS_UplinkConfig->transformPrecodingDisabled->scramblingID0;
      else if (NR_DMRS_UplinkConfig)
        scramblingid = NR_DMRS_UplinkConfig->transformPrecodingDisabled->scramblingID1;
      if (scramblingid == NULL)
        pusch_pdu->ul_dmrs_scrambling_id = *scc->physCellId;
      else
        pusch_pdu->ul_dmrs_scrambling_id = *scramblingid;
    }
    else {
      pusch_pdu->ul_dmrs_scrambling_id = *scc->physCellId;
      if (NR_DMRS_UplinkConfig &&
          NR_DMRS_UplinkConfig->transformPrecodingEnabled &&
          NR_DMRS_UplinkConfig->transformPrecodingEnabled->nPUSCH_Identity != NULL)
        pusch_pdu->pusch_identity = *NR_DMRS_UplinkConfig->transformPrecodingEnabled->nPUSCH_Identity;
      else if (NR_DMRS_UplinkConfig)
        pusch_pdu->pusch_identity = *scc->physCellId;
    }
    pusch_pdu->scid = 0;      // DMRS sequence initialization [TS38.211, sec 6.4.1.1.1]
    pusch_pdu->dmrs_ports = ((1<<sched_pusch->nrOfLayers) - 1);

    /* FAPI: Pusch Allocation in frequency domain */
    pusch_pdu->resource_alloc = 1; //type 1
    pusch_pdu->rb_start = sched_pusch->rbStart;
    pusch_pdu->rb_size = sched_pusch->rbSize;
    pusch_pdu->vrb_to_prb_mapping = 0;
    if (current_BWP->pusch_Config==NULL || current_BWP->pusch_Config->frequencyHopping==NULL)
      pusch_pdu->frequency_hopping = 0;
    else
      pusch_pdu->frequency_hopping = 1;

    /* FAPI: Resource Allocation in time domain */
    pusch_pdu->start_symbol_index = sched_pusch->tda_info.startSymbolIndex;
    pusch_pdu->nr_of_symbols = sched_pusch->tda_info.nrOfSymbols;

    /* PUSCH PDU */
    AssertFatal(cur_harq->round < nr_mac->ul_bler.harq_round_max, "Indexing nr_rv_round_map[%d] is out of bounds\n", cur_harq->round%4);
    pusch_pdu->pusch_data.rv_index = nr_rv_round_map[cur_harq->round%4];
    pusch_pdu->pusch_data.harq_process_id = harq_id;
    pusch_pdu->pusch_data.new_data_indicator = (cur_harq->round == 0) ? 1 : 0;  // not NDI but indicator for new transmission
    pusch_pdu->pusch_data.tb_size = sched_pusch->tb_size;
    pusch_pdu->pusch_data.num_cb = 0; //CBG not supported

    // Beamforming
    pusch_pdu->beamforming.num_prgs = 0;
    pusch_pdu->beamforming.prg_size = 0; // bwp_size;
    pusch_pdu->beamforming.dig_bf_interface = 1;
    pusch_pdu->beamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx = UE->UE_beam_index;

    pusch_pdu->maintenance_parms_v3.ldpcBaseGraph = get_BG(sched_pusch->tb_size<<3,sched_pusch->R);

    // Calacualte the normalized tx_power for PHR
    long *deltaMCS = current_BWP->pusch_Config ? current_BWP->pusch_Config->pusch_PowerControl->deltaMCS : NULL;
    int tbs_bits = pusch_pdu->pusch_data.tb_size << 3;

    sched_pusch->phr_txpower_calc = compute_ph_factor(current_BWP->scs,
                                                      tbs_bits,
                                                      sched_pusch->rbSize,
                                                      sched_pusch->nrOfLayers,
                                                      sched_pusch->tda_info.nrOfSymbols,
                                                      sched_pusch->dmrs_info.N_PRB_DMRS * sched_pusch->dmrs_info.num_dmrs_symb,
                                                      deltaMCS,
                                                      false);

    NR_UE_ServingCell_Info_t *sc_info = &UE->sc_info;
    if (sc_info->rateMatching_PUSCH) {
      // TBS_LBRM according to section 5.4.2.1 of 38.212
      long *maxMIMO_Layers = sc_info->maxMIMO_Layers_PUSCH;
      if (!maxMIMO_Layers)
        maxMIMO_Layers = current_BWP->pusch_Config->maxRank;
      AssertFatal (maxMIMO_Layers != NULL,"Option with max MIMO layers not configured is not supported\n");
      pusch_pdu->maintenance_parms_v3.tbSizeLbrmBytes =
          nr_compute_tbslbrm(current_BWP->mcs_table, sc_info->ul_bw_tbslbrm, *maxMIMO_Layers);
    } else
      pusch_pdu->maintenance_parms_v3.tbSizeLbrmBytes = 0;

    LOG_D(NR_MAC,"PUSCH PDU : data_scrambling_identity %x, dmrs_scrambling_id %x\n",pusch_pdu->data_scrambling_id,pusch_pdu->ul_dmrs_scrambling_id);
    /* TRANSFORM PRECODING --------------------------------------------------------*/

    if (pusch_pdu->transform_precoding == NR_PUSCH_Config__transformPrecoder_enabled){

      // U as specified in section 6.4.1.1.1.2 in 38.211, if sequence hopping and group hopping are disabled
      pusch_pdu->dfts_ofdm.low_papr_group_number = pusch_pdu->pusch_identity % 30;

      // V as specified in section 6.4.1.1.1.2 in 38.211 V = 0 if sequence hopping and group hopping are disabled
      if ((!NR_DMRS_UplinkConfig ||
          !NR_DMRS_UplinkConfig->transformPrecodingEnabled ||
          (!NR_DMRS_UplinkConfig->transformPrecodingEnabled->sequenceGroupHopping && !NR_DMRS_UplinkConfig->transformPrecodingEnabled->sequenceHopping)) &&
          !scc->uplinkConfigCommon->initialUplinkBWP->pusch_ConfigCommon->choice.setup->groupHoppingEnabledTransformPrecoding)
        pusch_pdu->dfts_ofdm.low_papr_sequence_number = 0;
      else
        AssertFatal(1==0,"Hopping mode is not supported in transform precoding\n");

      LOG_D(NR_MAC,"TRANSFORM PRECODING IS ENABLED. CDM groups: %d, U: %d MCS table: %d\n", pusch_pdu->num_dmrs_cdm_grps_no_data, pusch_pdu->dfts_ofdm.low_papr_group_number, current_BWP->mcs_table);
    }

    /*-----------------------------------------------------------------------------*/

    /* PUSCH PTRS */
    if (NR_DMRS_UplinkConfig && NR_DMRS_UplinkConfig->phaseTrackingRS != NULL) {
      bool valid_ptrs_setup = false;
      pusch_pdu->pusch_ptrs.ptrs_ports_list   = (nfapi_nr_ptrs_ports_t *) malloc(2*sizeof(nfapi_nr_ptrs_ports_t));
      valid_ptrs_setup = set_ul_ptrs_values(NR_DMRS_UplinkConfig->phaseTrackingRS->choice.setup,
                                            pusch_pdu->rb_size, pusch_pdu->mcs_index, pusch_pdu->mcs_table,
                                            &pusch_pdu->pusch_ptrs.ptrs_freq_density,&pusch_pdu->pusch_ptrs.ptrs_time_density,
                                            &pusch_pdu->pusch_ptrs.ptrs_ports_list->ptrs_re_offset,&pusch_pdu->pusch_ptrs.num_ptrs_ports,
                                            &pusch_pdu->pusch_ptrs.ul_ptrs_power, pusch_pdu->nr_of_symbols);
      if (valid_ptrs_setup==true) {
        pusch_pdu->pdu_bit_map |= PUSCH_PDU_BITMAP_PUSCH_PTRS; // enable PUSCH PTRS
      }
    }
    else{
      pusch_pdu->pdu_bit_map &= ~PUSCH_PDU_BITMAP_PUSCH_PTRS; // disable PUSCH PTRS
    }

    /* look up the PDCCH PDU for this BWP and CORESET. If it does not exist,
     * create it */
    NR_SearchSpace_t *ss = sched_ctrl->search_space;
    NR_ControlResourceSet_t *coreset = sched_ctrl->coreset;
    const int coresetid = coreset->controlResourceSetId;
    nfapi_nr_dl_tti_pdcch_pdu_rel15_t *pdcch_pdu = pdcch_pdu_coreset[coresetid];
    if (!pdcch_pdu) {
      nfapi_nr_ul_dci_request_pdus_t *ul_dci_request_pdu = &ul_dci_req->ul_dci_pdu_list[ul_dci_req->numPdus];
      memset(ul_dci_request_pdu, 0, sizeof(nfapi_nr_ul_dci_request_pdus_t));
      ul_dci_request_pdu->PDUType = NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE;
      ul_dci_request_pdu->PDUSize = (uint8_t)(4+sizeof(nfapi_nr_dl_tti_pdcch_pdu));
      pdcch_pdu = &ul_dci_request_pdu->pdcch_pdu.pdcch_pdu_rel15;
      ul_dci_req->numPdus += 1;
      nr_configure_pdcch(pdcch_pdu, coreset, &sched_ctrl->sched_pdcch, false);
      pdcch_pdu_coreset[coresetid] = pdcch_pdu;
    }

    LOG_D(NR_MAC,"Configuring ULDCI/PDCCH in %d.%d at CCE %d, rnti %04x\n", frame,slot,sched_ctrl->cce_index,rnti);

    /* Fill PDCCH DL DCI PDU */
    nfapi_nr_dl_dci_pdu_t *dci_pdu = &pdcch_pdu->dci_pdu[pdcch_pdu->numDlDci];
    pdcch_pdu->numDlDci++;
    dci_pdu->RNTI = rnti;
    if (coreset->pdcch_DMRS_ScramblingID &&
        ss->searchSpaceType->present == NR_SearchSpace__searchSpaceType_PR_ue_Specific) {
      dci_pdu->ScramblingId = *coreset->pdcch_DMRS_ScramblingID;
      dci_pdu->ScramblingRNTI = rnti;
    } else {
      dci_pdu->ScramblingId = *scc->physCellId;
      dci_pdu->ScramblingRNTI = 0;
    }
    dci_pdu->AggregationLevel = sched_ctrl->aggregation_level;
    dci_pdu->CceIndex = sched_ctrl->cce_index;
    dci_pdu->beta_PDCCH_1_0 = 0;
    dci_pdu->powerControlOffsetSS = 1;
    dci_pdu->precodingAndBeamforming.num_prgs = 0;
    dci_pdu->precodingAndBeamforming.prg_size = 0;
    dci_pdu->precodingAndBeamforming.dig_bf_interfaces = 1;
    dci_pdu->precodingAndBeamforming.prgs_list[0].pm_idx = 0;
    dci_pdu->precodingAndBeamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx = UE->UE_beam_index;

    dci_pdu_rel15_t uldci_payload;
    memset(&uldci_payload, 0, sizeof(uldci_payload));
    if (current_BWP->dci_format == NR_UL_DCI_FORMAT_0_1)
      LOG_D(NR_MAC_DCI,
            "add ul dci harq %d for %d.%d %d.%d round %d\n",
            harq_id,
            frame,
            slot,
            sched_pusch->frame,
            sched_pusch->slot,
            sched_ctrl->ul_harq_processes[harq_id].round);
    config_uldci(&UE->sc_info,
                 pusch_pdu,
                 &uldci_payload,
                 &sched_ctrl->srs_feedback,
                 tpmi,
                 sched_pusch->time_domain_allocation,
                 UE->UE_sched_ctrl.tpc0,
                 cur_harq->ndi,
                 current_BWP);

    // Reset TPC to 0 dB to not request new gain multiple times before computing new value for SNR
    UE->UE_sched_ctrl.tpc0 = 1;

    fill_dci_pdu_rel15(&UE->sc_info,
                       &UE->current_DL_BWP,
                       current_BWP,
                       dci_pdu,
                       &uldci_payload,
                       current_BWP->dci_format,
                       TYPE_C_RNTI_,
                       current_BWP->bwp_id,
                       ss,
                       coreset,
                       UE->pdsch_HARQ_ACK_Codebook,
                       nr_mac->cset0_bwp_size);

    memset(sched_pusch, 0, sizeof(*sched_pusch));
  }
}
