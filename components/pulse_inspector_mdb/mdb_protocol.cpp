#include "mdb_protocol.h"

#ifdef USE_ESP_IDF

namespace esphome {
namespace pulse_inspector_mdb {

const char *mdb_address_name(uint8_t base) {
  switch (base) {
    case MDB_ADDR_VMC: return "VMC";
    case MDB_ADDR_CHANGER: return "Changer";
    case MDB_ADDR_CASHLESS1: return "Cashless#1";
    case MDB_ADDR_COMMS: return "Comms";
    case MDB_ADDR_DISPLAY: return "Display";
    case MDB_ADDR_ENERGY: return "Energy";
    case MDB_ADDR_BV: return "BV";
    case MDB_ADDR_USD1: return "USD#1";
    case MDB_ADDR_AGE: return "AgeVerif";
    case MDB_ADDR_USD2: return "USD#2";
    case MDB_ADDR_CASHLESS2: return "Cashless#2";
    case MDB_ADDR_EXPERIMENTAL: return "Experimental";
    default: return "?";
  }
}

const char *mdb_subcommand_name(uint8_t base, uint8_t sub) {
  switch (base) {
    case MDB_ADDR_CHANGER:
      switch (sub) {
        case MDB_CHG_RESET: return "RESET";
        case MDB_CHG_SETUP: return "SETUP";
        case MDB_CHG_TUBE_STATUS: return "TUBE_STATUS";
        case MDB_CHG_POLL: return "POLL";
        case MDB_CHG_COIN_TYPE: return "COIN_TYPE";
        case MDB_CHG_DISPENSE: return "DISPENSE";
        case MDB_CHG_EXPANSION: return "EXPANSION";
      }
      break;
    case MDB_ADDR_CASHLESS1:
    case MDB_ADDR_CASHLESS2:
      switch (sub) {
        case MDB_CL_RESET: return "RESET";
        case MDB_CL_SETUP: return "SETUP";
        case MDB_CL_POLL: return "POLL";
        case MDB_CL_VEND: return "VEND";
        case MDB_CL_READER: return "READER";
        case MDB_CL_REVALUE: return "REVALUE";
        case MDB_CL_EXPANSION: return "EXPANSION";
      }
      break;
    case MDB_ADDR_BV:
      switch (sub) {
        case MDB_BV_RESET: return "RESET";
        case MDB_BV_SETUP: return "SETUP";
        case MDB_BV_SECURITY: return "SECURITY";
        case MDB_BV_POLL: return "POLL";
        case MDB_BV_BILL_TYPE: return "BILL_TYPE";
        case MDB_BV_ESCROW: return "ESCROW";
        case MDB_BV_STACKER: return "STACKER";
        case MDB_BV_EXPANSION: return "EXPANSION";
      }
      break;
  }
  return "cmd";
}

const char *mdb_bv_status_name(uint8_t byte) {
  switch (byte) {
    case 0x01: return "defective motor";
    case 0x02: return "sensor problem";
    case 0x03: return "validator busy";
    case 0x04: return "ROM checksum error";
    case 0x05: return "validator jammed";
    case 0x06: return "validator was reset";
    case 0x07: return "bill removed from escrow";
    case 0x08: return "cash box out of position";
    case 0x09: return "validator disabled";
    case 0x0A: return "invalid escrow request";
    case 0x0B: return "bill rejected";
    case 0x0C: return "possible credited bill removal";
    default: return nullptr;
  }
}

const char *mdb_changer_status_name(uint8_t byte) {
  switch (byte) {
    case 0x01: return "escrow request";
    case 0x02: return "changer payout busy";
    case 0x03: return "no credit";
    case 0x04: return "defective tube sensor";
    case 0x05: return "double arrival";
    case 0x06: return "acceptor unplugged";
    case 0x07: return "tube jam";
    case 0x08: return "ROM checksum error";
    case 0x09: return "coin routing error";
    case 0x0A: return "changer busy";
    case 0x0B: return "changer was reset";
    case 0x0C: return "coin jam";
    case 0x0D: return "possible credited coin removal";
    default: return nullptr;
  }
}

const char *mdb_cashless_reply_name(uint8_t byte) {
  switch (byte) {
    case MDB_CL_REPLY_JUST_RESET: return "JUST_RESET";
    case MDB_CL_REPLY_CONFIG: return "CONFIG";
    case MDB_CL_REPLY_DISPLAY: return "DISPLAY_REQUEST";
    case MDB_CL_REPLY_BEGIN_SESSION: return "BEGIN_SESSION";
    case MDB_CL_REPLY_SESSION_CANCEL_REQ: return "SESSION_CANCEL_REQ";
    case MDB_CL_REPLY_VEND_APPROVED: return "VEND_APPROVED";
    case MDB_CL_REPLY_VEND_DENIED: return "VEND_DENIED";
    case MDB_CL_REPLY_END_SESSION: return "END_SESSION";
    case MDB_CL_REPLY_CANCELLED: return "CANCELLED";
    case MDB_CL_REPLY_PERIPHERAL_ID: return "PERIPHERAL_ID";
    case MDB_CL_REPLY_MALFUNCTION: return "MALFUNCTION";
    case MDB_CL_REPLY_CMD_OUT_OF_SEQ: return "CMD_OUT_OF_SEQ";
    case MDB_CL_REPLY_REVALUE_APPROVED: return "REVALUE_APPROVED";
    case MDB_CL_REPLY_REVALUE_DENIED: return "REVALUE_DENIED";
    case MDB_CL_REPLY_REVALUE_LIMIT: return "REVALUE_LIMIT";
    case MDB_CL_REPLY_TIME_DATE: return "TIME_DATE_REQ";
    case MDB_CL_REPLY_DIAGNOSTIC: return "DIAGNOSTIC";
    default: return nullptr;
  }
}

const char *mdb_vend_subcmd_name(uint8_t byte) {
  switch (byte) {
    case MDB_CL_VEND_REQUEST: return "REQUEST";
    case MDB_CL_VEND_CANCEL: return "CANCEL";
    case MDB_CL_VEND_SUCCESS: return "SUCCESS";
    case MDB_CL_VEND_FAILURE: return "FAILURE";
    case MDB_CL_VEND_SESSION_COMPLETE: return "SESSION_COMPLETE";
    case MDB_CL_VEND_CASH_SALE: return "CASH_SALE";
    case MDB_CL_VEND_NEGATIVE: return "NEGATIVE";
    default: return "?";
  }
}

const char *mdb_event_kind_name(MdbEvent::Kind k) {
  switch (k) {
    case MdbEvent::BEGIN_SESSION: return "BEGIN_SESSION";
    case MdbEvent::END_SESSION: return "END_SESSION";
    case MdbEvent::SESSION_CANCEL_REQUEST: return "SESSION_CANCEL_REQUEST";
    case MdbEvent::VEND_REQUEST: return "VEND_REQUEST";
    case MdbEvent::VEND_APPROVED: return "VEND_APPROVED";
    case MdbEvent::VEND_DENIED: return "VEND_DENIED";
    case MdbEvent::VEND_SUCCESS: return "VEND_SUCCESS";
    case MdbEvent::VEND_FAILURE: return "VEND_FAILURE";
    case MdbEvent::CASH_SALE: return "CASH_SALE";
    case MdbEvent::BV_BILL_ACCEPTED: return "BV_BILL_ACCEPTED";
    case MdbEvent::BV_BILL_REJECTED: return "BV_BILL_REJECTED";
    case MdbEvent::BV_STACKED: return "BV_STACKED";
    case MdbEvent::BV_RESET: return "BV_RESET";
    case MdbEvent::BV_ERROR: return "BV_ERROR";
    case MdbEvent::BV_STATUS: return "BV_STATUS";
    case MdbEvent::BV_BILL_TYPE_CMD: return "BV_BILL_TYPE_CMD";
    case MdbEvent::BV_ESCROW_CMD: return "BV_ESCROW_CMD";
    case MdbEvent::SALE_CYCLE_BEGIN: return "SALE_CYCLE_BEGIN";
    case MdbEvent::SALE_CYCLE_END: return "SALE_CYCLE_END";
    case MdbEvent::CHG_COIN_DEPOSITED: return "CHG_COIN_DEPOSITED";
    case MdbEvent::CHG_COIN_DISPENSED: return "CHG_COIN_DISPENSED";
    case MdbEvent::CHG_RESET: return "CHG_RESET";
    case MdbEvent::CHG_ERROR: return "CHG_ERROR";
    case MdbEvent::CHG_SLUG: return "CHG_SLUG";
    case MdbEvent::CHG_TUBES_UPDATE: return "CHG_TUBES_UPDATE";
    case MdbEvent::CL_RESET: return "CL_RESET";
    case MdbEvent::CL_MALFUNCTION: return "CL_MALFUNCTION";
    case MdbEvent::CL_DIAGNOSTIC: return "CL_DIAGNOSTIC";
    case MdbEvent::CL_DISPLAY_REQUEST: return "CL_DISPLAY_REQUEST";
    case MdbEvent::RAW_MASTER_FRAME: return "RAW_MASTER_FRAME";
    case MdbEvent::RAW_SLAVE_FRAME: return "RAW_SLAVE_FRAME";
  }
  return "?";
}

}  // namespace pulse_inspector_mdb
}  // namespace esphome

#endif  // USE_ESP_IDF
